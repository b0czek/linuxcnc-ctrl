#include <grpcpp/grpcpp.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "grpc_live_support.hpp"
#include "linuxcnc/v1/hal.grpc.pb.h"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc/v1/program.grpc.pb.h"
#include "linuxcnc/v1/scope.grpc.pb.h"
#include "linuxcnc/v1/websocket.pb.h"
#include "workspace_archive_fixture.hpp"

using namespace grpc_live;
int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: linuxcnc-grpc-live-integration ENDPOINT GCODE_FIXTURE "
                 "[TELEMETRY_ENDPOINT] [--batch-size=N] "
                 "[--hold-shutdown|--probe-reacquire]\n";
    return 2;
  }
  const std::string endpoint = argc > 1 ? argv[1] : "127.0.0.1:50051";
  std::string telemetry_endpoint = "127.0.0.1:50052";
  std::string mode;
  std::size_t batch_limit = 128;
  for (int index = 3; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument.rfind("--batch-size=", 0) == 0) {
      batch_limit = static_cast<std::size_t>(
          std::stoul(argument.substr(std::string("--batch-size=").size())));
    } else if (argument.rfind("--", 0) == 0) {
      mode = argument;
    } else {
      telemetry_endpoint = argument;
    }
  }
  if (mode == "--hold-shutdown") return hold_shutdown(endpoint);
  if (mode == "--probe-reacquire") return probe_reacquire(endpoint);
  if (!mode.empty()) {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  const std::string gcode = read_file(argv[2]);
  const auto channel = make_channel(endpoint);
  const auto machine = linuxcnc::v1::MachineService::NewStub(channel);

  const auto baseline = get_status_with_retry(machine.get());
  assert(baseline.sequence() != 0);
  assert(baseline.has_status());
  assert(baseline.status().has_task());
  assert(baseline.status().has_motion());

  const auto target_optional_stop =
      !baseline.status().task().optional_stop_state();
  const auto accepted = set_optional_stop(machine.get(), target_optional_stop,
                                          linuxcnc::v1::WAIT_POLICY_ACCEPTED);
  assert(accepted.status() == linuxcnc::v1::RCS_STATUS_EXEC ||
         accepted.status() == linuxcnc::v1::RCS_STATUS_DONE);
  const auto completed = set_optional_stop(machine.get(), target_optional_stop,
                                           linuxcnc::v1::WAIT_POLICY_COMPLETED);
  assert(completed.status() == linuxcnc::v1::RCS_STATUS_DONE);
  const auto changed =
      wait_for_optional_stop(machine.get(), target_optional_stop);
  assert(changed.status().task().optional_stop_state() == target_optional_stop);
  assert(changed.status().echo_serial_number() ==
         static_cast<std::int64_t>(completed.command_sequence()));

  // The first event is a typed replay from the baseline sequence. This
  // verifies that a status change made through the command queue is visible
  // without requiring a second full snapshot.
  grpc::ClientContext watch_context;
  watch_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(5));
  linuxcnc::v1::WatchStatusRequest watch_request;
  watch_request.set_after_sequence(baseline.sequence());
  auto watch = machine->WatchStatus(&watch_context, watch_request);
  linuxcnc::v1::WatchStatusEvent event;
  assert(watch->Read(&event));
  assert(event.has_replay());
  assert(event.replay().from_sequence() == baseline.sequence());
  assert(event.replay().has_snapshot());
  assert(event.replay().deltas_size() > 0);
  bool found_optional_stop_delta = false;
  for (const auto& delta : event.replay().deltas()) {
    if (delta.has_task() && delta.task().has_optional_stop_state()) {
      found_optional_stop_delta = true;
      break;
    }
  }
  assert(found_optional_stop_delta);
  watch_context.TryCancel();
  (void)watch->Finish();

  // Position history configuration remains on the gRPC control plane.
  linuxcnc::v1::PositionHistoryConfig position_config;
  position_config.set_enabled(true);
  position_config.set_capacity(64);
  position_config.set_sample_period_ms(10);
  google::protobuf::Empty empty;
  grpc::ClientContext configure_position_context;
  const auto configure_position_status = machine->ConfigurePositionHistory(
      &configure_position_context, position_config, &empty);
  assert(configure_position_status.ok());
  verify_position_telemetry(telemetry_endpoint);

  // A slow position-history cadence must not throttle the independent status
  // watcher. The old shared sleep made this update take up to 60 seconds.
  position_config.set_sample_period_ms(60000);
  grpc::ClientContext slow_position_context;
  assert(machine
             ->ConfigurePositionHistory(&slow_position_context, position_config,
                                        &empty)
             .ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto before_slow_position_command =
      get_status_with_retry(machine.get());
  grpc::ClientContext independent_watch_context;
  independent_watch_context.set_deadline(std::chrono::system_clock::now() +
                                         std::chrono::seconds(2));
  linuxcnc::v1::WatchStatusRequest independent_watch_request;
  independent_watch_request.set_after_sequence(
      before_slow_position_command.sequence());
  auto independent_watch = machine->WatchStatus(&independent_watch_context,
                                                independent_watch_request);
  const bool independent_optional_stop =
      !before_slow_position_command.status().task().optional_stop_state();
  (void)set_optional_stop(machine.get(), independent_optional_stop,
                          linuxcnc::v1::WAIT_POLICY_COMPLETED);
  linuxcnc::v1::WatchStatusEvent independent_event;
  assert(independent_watch->Read(&independent_event));
  independent_watch_context.TryCancel();
  (void)independent_watch->Finish();
  position_config.set_sample_period_ms(10);
  grpc::ClientContext restore_position_context;
  assert(machine
             ->ConfigurePositionHistory(&restore_position_context,
                                        position_config, &empty)
             .ok());

  ExecuteCommandRequest reset_estop;
  reset_estop.mutable_set_state()->set_state(
      linuxcnc::v1::TASK_STATE_ESTOP_RESET);
  (void)execute_completed(machine.get(), std::move(reset_estop));
  ExecuteCommandRequest machine_on;
  machine_on.mutable_set_state()->set_state(linuxcnc::v1::TASK_STATE_ON);
  (void)execute_completed(machine.get(), std::move(machine_on));
  ExecuteCommandRequest mdi_mode;
  mdi_mode.mutable_set_task_mode()->set_mode(linuxcnc::v1::TASK_MODE_MDI);
  (void)execute_completed(machine.get(), std::move(mdi_mode));
  verify_moving_status_cadence(machine.get());
  ExecuteCommandRequest mdi_move;
  mdi_move.mutable_mdi()->set_command("G0 X1");
  (void)execute_completed(machine.get(), std::move(mdi_move));

  // Completion observation must not occupy the command writer. A dwell keeps
  // an MDI command executing while an abort is admitted and written.
  std::atomic<bool> dwell_started{false};
  grpc::Status dwell_status;
  ExecuteCommandResponse dwell_response;
  std::thread dwell([&] {
    ExecuteCommandRequest request;
    request.set_wait_policy(linuxcnc::v1::WAIT_POLICY_COMPLETED);
    request.mutable_mdi()->set_command("G4 P2");
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));
    dwell_started = true;
    dwell_status = machine->ExecuteCommand(&context, request, &dwell_response);
  });
  while (!dwell_started.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ExecuteCommandRequest abort;
  abort.set_wait_policy(linuxcnc::v1::WAIT_POLICY_ACCEPTED);
  abort.mutable_abort_task();
  grpc::ClientContext abort_context;
  abort_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(1));
  ExecuteCommandResponse abort_response;
  const auto abort_started = std::chrono::steady_clock::now();
  const auto abort_status =
      machine->ExecuteCommand(&abort_context, abort, &abort_response);
  const auto abort_elapsed = std::chrono::steady_clock::now() - abort_started;
  assert(abort_status.ok());
  assert(abort_response.command_sequence() != 0);
  assert(abort_elapsed < std::chrono::seconds(1));
  dwell.join();
  assert(dwell_status.ok());

  // Exercise one safe representative from the remaining command families.
  // The protobuf setters are the command catalog: this table deliberately
  // does not duplicate command-case numbers or names.
  const std::pair<const char*, std::function<void(ExecuteCommandRequest&)>>
      commands[] = {
          {"task", [](auto& request) { request.mutable_task_plan_synch(); }},
          {"trajectory",
           [](auto& request) {
             request.mutable_set_feed_rate()->set_scale(1.0);
           }},
          {"jog",
           [](auto& request) {
             request.mutable_jog_stop()->set_axis_or_joint_index(0);
             request.mutable_jog_stop()->set_is_joint_jog(false);
           }},
          {"spindle",
           [](auto& request) {
             request.mutable_spindle_off()->set_spindle_index(0);
           }},
          {"coolant",
           [](auto& request) { request.mutable_set_mist()->set_on(false); }},
          {"tool", [](auto& request) { request.mutable_load_tool_table(); }},
          {"io",
           [](auto& request) {
             request.mutable_set_digital_output()->set_index(0);
             request.mutable_set_digital_output()->set_value(false);
           }},
          {"debug",
           [](auto& request) {
             request.mutable_set_debug_level()->set_level(0);
           }},
          {"operator-message",
           [](auto& request) {
             request.mutable_send_operator_text()->set_message(
                 "linuxcnc-grpc live acceptance");
           }},
      };
  for (const auto& [family, prepare] : commands) {
    ExecuteCommandRequest request;
    prepare(request);
    const auto response = execute_completed(machine.get(), std::move(request));
    if (response.command_sequence() == 0) {
      std::cerr << "missing command sequence for " << family << " family\n";
      return 1;
    }
  }

  const auto tool_status = get_status_with_retry(machine.get());
  linuxcnc::v1::ToolEntry expected_tool;
  for (const auto& tool : tool_status.status().tool_table()) {
    if (tool.tool_no() == 1) expected_tool = tool;
  }
  assert(expected_tool.tool_no() == 1);
  ExecuteCommandRequest partial_tool_update;
  auto* partial_tool = partial_tool_update.mutable_set_tool()->mutable_tool();
  partial_tool->set_tool_no(1);
  partial_tool->mutable_wear_offset()->add_values(0.25);
  (void)execute_completed(machine.get(), std::move(partial_tool_update));
  expected_tool.mutable_wear_offset()->set_values(0, 0.25);
  const auto tool_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool tool_updated = false;
  while (std::chrono::steady_clock::now() < tool_deadline && !tool_updated) {
    const auto status = get_status_with_retry(machine.get());
    for (const auto& tool : status.status().tool_table()) {
      if (tool.tool_no() == 1 &&
          tool.SerializeAsString() == expected_tool.SerializeAsString()) {
        tool_updated = true;
        break;
      }
    }
    if (!tool_updated)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(tool_updated);

  ExecuteCommandRequest create_tool;
  auto* created_tool = create_tool.mutable_set_tool()->mutable_tool();
  created_tool->set_tool_no(77);
  created_tool->set_pocket_no(42);
  created_tool->set_diameter(7.7);
  auto create_response =
      execute_completed(machine.get(), std::move(create_tool));
  assert(create_response.command_sequence() != 0);
  const auto created_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool tool_created = false;
  while (std::chrono::steady_clock::now() < created_deadline && !tool_created) {
    const auto status = get_status_with_retry(machine.get());
    for (const auto& tool : status.status().tool_table()) {
      if (tool.tool_no() == 77 && tool.pocket_no() == 42 &&
          tool.diameter() == 7.7) {
        tool_created = true;
        break;
      }
    }
    if (!tool_created)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(tool_created);

  ExecuteCommandRequest move_tool;
  auto* moved_tool = move_tool.mutable_set_tool()->mutable_tool();
  moved_tool->set_tool_no(77);
  moved_tool->set_pocket_no(43);
  (void)execute_completed(machine.get(), std::move(move_tool));
  const auto moved_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool tool_moved = false;
  while (std::chrono::steady_clock::now() < moved_deadline && !tool_moved) {
    const auto status = get_status_with_retry(machine.get());
    for (const auto& tool : status.status().tool_table()) {
      if (tool.tool_no() == 77 && tool.pocket_no() == 43) {
        tool_moved = true;
        break;
      }
    }
    if (!tool_moved) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(tool_moved);

  ExecuteCommandRequest load_created_tool;
  load_created_tool.mutable_mdi()->set_command("T77 M6");
  (void)execute_completed(machine.get(), std::move(load_created_tool));
  const auto loaded_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool tool_loaded = false;
  while (std::chrono::steady_clock::now() < loaded_deadline && !tool_loaded) {
    const auto status = get_status_with_retry(machine.get());
    tool_loaded = status.status().io().tool().tool_in_spindle() == 77 &&
                  status.status().tool_table(0).tool_no() == 77;
    if (!tool_loaded)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(tool_loaded);

  ExecuteCommandRequest update_loaded_tool;
  auto* loaded_tool = update_loaded_tool.mutable_set_tool()->mutable_tool();
  loaded_tool->set_tool_no(77);
  loaded_tool->set_diameter(8.8);
  loaded_tool->set_comment("updated while loaded");
  (void)execute_completed(machine.get(), std::move(update_loaded_tool));
  const auto updated_loaded_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool loaded_tool_updated = false;
  while (std::chrono::steady_clock::now() < updated_loaded_deadline &&
         !loaded_tool_updated) {
    const auto status = get_status_with_retry(machine.get());
    loaded_tool_updated =
        status.status().io().tool().tool_in_spindle() == 77 &&
        status.status().tool_table_size() > 0 &&
        status.status().tool_table(0).tool_no() == 77 &&
        status.status().tool_table(0).diameter() == 8.8 &&
        status.status().tool_table(0).comment() == "updated while loaded";
    if (!loaded_tool_updated)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(loaded_tool_updated);

  ExecuteCommandRequest unload_created_tool;
  unload_created_tool.mutable_mdi()->set_command("T0 M6");
  (void)execute_completed(machine.get(), std::move(unload_created_tool));
  ExecuteCommandRequest delete_tool;
  delete_tool.mutable_delete_tool()->set_tool_no(77);
  const auto delete_response =
      execute_completed(machine.get(), std::move(delete_tool));
  assert(delete_response.command_sequence() != 0);
  const auto deleted_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool tool_deleted = false;
  while (std::chrono::steady_clock::now() < deleted_deadline && !tool_deleted) {
    const auto status = get_status_with_retry(machine.get());
    tool_deleted = true;
    for (const auto& tool : status.status().tool_table()) {
      if (tool.tool_no() == 77) {
        tool_deleted = false;
        break;
      }
    }
    if (!tool_deleted)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  assert(tool_deleted);

  expect_command_error(machine.get(), ExecuteCommandRequest{},
                       grpc::StatusCode::INVALID_ARGUMENT);
  ExecuteCommandRequest missing_tool;
  missing_tool.mutable_set_tool();
  expect_command_error(machine.get(), missing_tool,
                       grpc::StatusCode::INVALID_ARGUMENT);
  ExecuteCommandRequest oversized_tool;
  auto* oversized = oversized_tool.mutable_set_tool()->mutable_tool();
  oversized->set_tool_no(88);
  oversized->set_pocket_no(88);
  for (int axis = 0; axis < 10; ++axis)
    oversized->mutable_offset()->add_values(axis);
  expect_command_error(machine.get(), oversized_tool,
                       grpc::StatusCode::INVALID_ARGUMENT);

  grpc::ClientContext clear_position_context;
  const auto clear_position_status =
      machine->ClearPositionHistory(&clear_position_context, empty, &empty);
  assert(clear_position_status.ok());

  // Upload and parse a repo-owned native fixture through the real workspace
  // store and the serialized rs274 interpreter.
  const auto program = linuxcnc::v1::ProgramService::NewStub(channel);
  const auto workspace =
      upload_program(program.get(), "simple-linear.ngc", gcode);
  assert(!workspace.workspace_id().empty());
  assert(workspace.extracted_bytes() == gcode.size());
  assert(workspace.entries() == 1);

  verify_preview_stream(telemetry_endpoint, workspace.workspace_id(),
                        "simple-linear.ngc", batch_limit);

  verify_chunked_preview_stream(program.get(), telemetry_endpoint, batch_limit);

  linuxcnc::v1::DeleteWorkspaceRequest delete_workspace_request;
  delete_workspace_request.set_workspace_id(workspace.workspace_id());
  grpc::ClientContext delete_workspace_context;
  const auto delete_workspace_status = program->DeleteWorkspace(
      &delete_workspace_context, delete_workspace_request, &empty);
  assert(delete_workspace_status.ok());

  if (!verify_hal_and_scope(channel, telemetry_endpoint)) return 1;

  // Saturation deliberately pressures LinuxCNC's command channel, so keep it
  // last while still exercising the real RPC admission boundary.
  verify_priority_admission(machine.get());

  std::cout << "native LinuxCNC gRPC live integration passed\n";
  return 0;
}

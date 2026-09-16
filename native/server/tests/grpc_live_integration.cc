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

#include "linuxcnc/v1/hal.grpc.pb.h"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc/v1/program.grpc.pb.h"
#include "linuxcnc/v1/scope.grpc.pb.h"
#include "linuxcnc/v1/websocket.pb.h"
#include "workspace_archive_fixture.hpp"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

using linuxcnc::v1::ExecuteCommandRequest;
using linuxcnc::v1::ExecuteCommandResponse;
using linuxcnc::v1::GetStatusResponse;

std::shared_ptr<grpc::Channel> make_channel(const std::string& endpoint) {
  return grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
}
std::pair<std::string, std::string> split_endpoint(
    const std::string& endpoint) {
  const auto separator = endpoint.rfind(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == endpoint.size()) {
    std::cerr << "Invalid telemetry endpoint: " << endpoint << "\n";
    std::abort();
  }
  return {endpoint.substr(0, separator), endpoint.substr(separator + 1)};
}

void verify_position_telemetry(const std::string& endpoint) {
  const auto [host, port] = split_endpoint(endpoint);
  asio::io_context io;
  tcp::resolver resolver(io);
  websocket::stream<beast::tcp_stream> socket(io);
  beast::get_lowest_layer(socket).expires_after(std::chrono::seconds(5));
  beast::get_lowest_layer(socket).connect(resolver.resolve(host, port));
  socket.handshake(host, "/v1/position-history");
  beast::flat_buffer buffer;
  socket.read(buffer);
  std::vector<std::uint8_t> bytes(buffer.size());
  asio::buffer_copy(asio::buffer(bytes), buffer.data());
  linuxcnc::v1::PositionHistoryFrame frame;
  assert(frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  assert(frame.kind() == linuxcnc::v1::FRAME_KIND_REPLACEMENT);
  socket.close(websocket::close_code::normal);
}

linuxcnc::v1::HalValueFrame read_hal_value_frame(
    websocket::stream<beast::tcp_stream>* socket) {
  beast::flat_buffer buffer;
  socket->read(buffer);
  std::vector<std::uint8_t> bytes(buffer.size());
  asio::buffer_copy(asio::buffer(bytes), buffer.data());
  linuxcnc::v1::HalValueFrame frame;
  assert(frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  return frame;
}

void verify_hal_value_subscription_lifecycle(
    linuxcnc::v1::HalService::Stub* hal, const std::string& telemetry_endpoint,
    const std::string& pin_name) {
  linuxcnc::v1::CreateHalValueSubscriptionRequest create_request;
  create_request.set_sample_period_ms(50);
  auto* create_item = create_request.add_items();
  create_item->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  create_item->set_name(pin_name);
  grpc::ClientContext create_context;
  linuxcnc::v1::HalValueSubscription created;
  assert(hal->CreateValueSubscription(&create_context, create_request, &created)
             .ok());
  assert(created.revision() == 1);
  assert(!created.websocket_path().empty());

  const auto [host, port] = split_endpoint(telemetry_endpoint);
  asio::io_context first_io;
  tcp::resolver first_resolver(first_io);
  websocket::stream<beast::tcp_stream> first_socket(first_io);
  beast::get_lowest_layer(first_socket).expires_after(std::chrono::seconds(5));
  beast::get_lowest_layer(first_socket)
      .connect(first_resolver.resolve(host, port));
  first_socket.handshake(host, created.websocket_path());
  const auto initial = read_hal_value_frame(&first_socket);
  assert(initial.kind() == linuxcnc::v1::FRAME_KIND_REPLACEMENT);
  assert(initial.revision() == created.revision());

  linuxcnc::v1::UpdateHalValueSubscriptionRequest update_request;
  update_request.set_subscription_id(created.subscription_id());
  update_request.set_expected_revision(created.revision());
  update_request.set_sample_period_ms(100);
  auto* update_item = update_request.add_items();
  update_item->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  update_item->set_name(pin_name);
  grpc::ClientContext update_context;
  linuxcnc::v1::HalValueSubscription updated;
  assert(hal->UpdateValueSubscription(&update_context, update_request, &updated)
             .ok());
  assert(updated.revision() == created.revision() + 1);
  assert(updated.websocket_path() == created.websocket_path());
  const auto replacement = read_hal_value_frame(&first_socket);
  assert(replacement.kind() == linuxcnc::v1::FRAME_KIND_REPLACEMENT);
  assert(replacement.revision() == updated.revision());

  first_socket.close(websocket::close_code::normal);

  asio::io_context second_io;
  tcp::resolver second_resolver(second_io);
  websocket::stream<beast::tcp_stream> second_socket(second_io);
  beast::get_lowest_layer(second_socket).expires_after(std::chrono::seconds(5));
  beast::get_lowest_layer(second_socket)
      .connect(second_resolver.resolve(host, port));
  // A normal close handshake completes only after the server has released the
  // attachment, so the stable path can reconnect immediately.
  second_socket.handshake(host, created.websocket_path());
  const auto reconnected = read_hal_value_frame(&second_socket);
  assert(reconnected.kind() == linuxcnc::v1::FRAME_KIND_REPLACEMENT);
  assert(reconnected.revision() == updated.revision());
  second_socket.close(websocket::close_code::normal);

  linuxcnc::v1::DeleteHalValueSubscriptionRequest delete_request;
  delete_request.set_subscription_id(created.subscription_id());
  grpc::ClientContext delete_context;
  google::protobuf::Empty empty;
  assert(hal->DeleteValueSubscription(&delete_context, delete_request, &empty)
             .ok());
}

GetStatusResponse get_status_with_retry(
    linuxcnc::v1::MachineService::Stub* machine) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(2));
    GetStatusResponse response;
    const auto status = machine->GetStatus(&context, {}, &response);
    if (status.ok()) return response;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cerr << "LinuxCNC status did not become available within 15 seconds\n";
  std::abort();
}

GetStatusResponse wait_for_optional_stop(
    linuxcnc::v1::MachineService::Stub* machine, bool expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto response = get_status_with_retry(machine);
    if (response.has_status() && response.status().has_task() &&
        response.status().task().optional_stop_state() == expected) {
      return response;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::cerr << "Optional-stop status did not converge within 5 seconds\n";
  std::abort();
}

ExecuteCommandResponse set_optional_stop(
    linuxcnc::v1::MachineService::Stub* machine, bool enabled,
    linuxcnc::v1::WaitPolicy wait_policy) {
  ExecuteCommandRequest request;
  request.set_wait_policy(wait_policy);
  request.mutable_set_optional_stop()->set_enable(enabled);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  ExecuteCommandResponse response;
  const auto status = machine->ExecuteCommand(&context, request, &response);
  if (!status.ok()) {
    std::cerr << "ExecuteCommand failed: " << status.error_message() << "\n";
    std::abort();
  }
  assert(response.command_sequence() != 0);
  return response;
}

ExecuteCommandResponse execute_completed(
    linuxcnc::v1::MachineService::Stub* machine,
    ExecuteCommandRequest request) {
  request.set_wait_policy(linuxcnc::v1::WAIT_POLICY_COMPLETED);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  ExecuteCommandResponse response;
  const auto status = machine->ExecuteCommand(&context, request, &response);
  if (!status.ok()) {
    std::cerr << "ExecuteCommand failed: " << status.error_message() << "\n";
    std::abort();
  }
  assert(response.status() == linuxcnc::v1::RCS_STATUS_DONE);
  return response;
}

ExecuteCommandResponse execute_accepted(
    linuxcnc::v1::MachineService::Stub* machine,
    ExecuteCommandRequest request) {
  request.set_wait_policy(linuxcnc::v1::WAIT_POLICY_ACCEPTED);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  ExecuteCommandResponse response;
  const auto status = machine->ExecuteCommand(&context, request, &response);
  if (!status.ok()) {
    std::cerr << "ExecuteCommand failed: " << status.error_message() << "\n";
    std::abort();
  }
  assert(response.status() == linuxcnc::v1::RCS_STATUS_EXEC ||
         response.status() == linuxcnc::v1::RCS_STATUS_DONE);
  return response;
}

void verify_moving_status_cadence(linuxcnc::v1::MachineService::Stub* machine) {
  const auto baseline = get_status_with_retry(machine);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  linuxcnc::v1::WatchStatusRequest request;
  request.set_after_sequence(baseline.sequence());
  auto watch = machine->WatchStatus(&context, request);

  ExecuteCommandRequest move;
  move.mutable_mdi()->set_command("G1 X20 F600");
  (void)execute_accepted(machine, std::move(move));

  std::vector<std::chrono::steady_clock::time_point> arrivals;
  linuxcnc::v1::WatchStatusEvent event;
  while (arrivals.size() < 20 && watch->Read(&event)) {
    if ((event.has_delta() && event.delta().has_motion() &&
         event.delta().motion().has_traj()) ||
        (event.has_replay() && event.replay().deltas_size() > 0)) {
      arrivals.push_back(std::chrono::steady_clock::now());
    }
  }
  context.TryCancel();
  (void)watch->Finish();
  assert(arrivals.size() == 20);
  auto maximum_gap = std::chrono::steady_clock::duration::zero();
  for (std::size_t index = 1; index < arrivals.size(); ++index)
    maximum_gap = std::max(maximum_gap, arrivals[index] - arrivals[index - 1]);
  const auto maximum_gap_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(maximum_gap);
  std::cout << "moving status maximum gap: " << maximum_gap_ms.count()
            << " ms\n";
  assert(maximum_gap_ms < std::chrono::milliseconds(120));
}

void expect_command_error(linuxcnc::v1::MachineService::Stub* machine,
                          const ExecuteCommandRequest& request,
                          grpc::StatusCode expected) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(2));
  ExecuteCommandResponse response;
  const auto status = machine->ExecuteCommand(&context, request, &response);
  assert(!status.ok());
  assert(status.error_code() == expected);
}

void verify_priority_admission(linuxcnc::v1::MachineService::Stub* machine) {
  constexpr int kSubmitters = 256;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> normal_capacity_exhausted{false};
  std::atomic<bool> unexpected_failure{false};
  std::vector<std::thread> submitters;
  submitters.reserve(kSubmitters);
  for (int index = 0; index < kSubmitters; ++index) {
    submitters.emplace_back([&] {
      ++ready;
      while (!start.load()) std::this_thread::yield();
      for (int attempt = 0; attempt < 20 && !stop.load(); ++attempt) {
        ExecuteCommandRequest request;
        request.set_wait_policy(linuxcnc::v1::WAIT_POLICY_ACCEPTED);
        request.mutable_task_plan_synch();
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(2));
        ExecuteCommandResponse response;
        const auto status =
            machine->ExecuteCommand(&context, request, &response);
        if (status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {
          normal_capacity_exhausted = true;
        } else if (!status.ok()) {
          unexpected_failure = true;
        }
      }
    });
  }
  while (ready.load() != kSubmitters) std::this_thread::yield();
  start = true;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!normal_capacity_exhausted.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  grpc::Status priority_status;
  ExecuteCommandResponse priority_response;
  if (normal_capacity_exhausted.load()) {
    ExecuteCommandRequest abort;
    abort.set_wait_policy(linuxcnc::v1::WAIT_POLICY_ACCEPTED);
    abort.mutable_abort_task();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(2));
    priority_status =
        machine->ExecuteCommand(&context, abort, &priority_response);
  }
  stop = true;
  for (auto& submitter : submitters) submitter.join();

  assert(normal_capacity_exhausted.load());
  assert(!unexpected_failure.load());
  assert(priority_status.ok());
  assert(priority_response.status() == linuxcnc::v1::RCS_STATUS_EXEC ||
         priority_response.status() == linuxcnc::v1::RCS_STATUS_DONE ||
         priority_response.status() == linuxcnc::v1::RCS_STATUS_ERROR);
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "Unable to read native G-code fixture: " << path << "\n";
    std::abort();
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void require_shutdown_status(const grpc::Status& status, const char* name) {
  if (status.error_code() != grpc::StatusCode::UNAVAILABLE &&
      status.error_code() != grpc::StatusCode::CANCELLED) {
    std::cerr << name << " ended with unexpected status " << status.error_code()
              << ": " << status.error_message() << "\n";
    std::abort();
  }
}

constexpr std::size_t kChunkedPreviewFeedCount = 257;

std::string make_chunked_preview_gcode() {
  std::string result{"G21 G90\nG0 X0 Y0 Z0\n"};
  for (std::size_t index = 1; index <= kChunkedPreviewFeedCount; ++index) {
    result += "G1 X" + std::to_string(index % 97) + " Y" +
              std::to_string((index * 7) % 89) + " F100\n";
  }
  result += "M2\n";
  return result;
}

linuxcnc::v1::UploadWorkspaceResponse upload_program(
    linuxcnc::v1::ProgramService::Stub* program, const std::string& path,
    const std::string& contents) {
  grpc::ClientContext upload_context;
  upload_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(15));
  linuxcnc::v1::UploadWorkspaceResponse response;
  auto upload = program->UploadWorkspace(&upload_context, &response);
  linuxcnc::v1::UploadWorkspaceRequest request;
  request.set_archive_chunk(workspace_archive_fixture(path, contents));
  assert(upload->Write(request));
  assert(upload->WritesDone());
  assert(upload->Finish().ok());
  return response;
}

void delete_workspace(linuxcnc::v1::ProgramService::Stub* program,
                      const std::string& workspace_id) {
  linuxcnc::v1::DeleteWorkspaceRequest request;
  request.set_workspace_id(workspace_id);
  grpc::ClientContext context;
  google::protobuf::Empty response;
  assert(program->DeleteWorkspace(&context, request, &response).ok());
}

void verify_preview_stream(const std::string& telemetry_endpoint,
                           const std::string& workspace_id,
                           const std::string& relative_path,
                           std::size_t batch_limit) {
  const auto [host, port] = split_endpoint(telemetry_endpoint);
  asio::io_context io;
  tcp::resolver resolver(io);
  websocket::stream<beast::tcp_stream> socket(io);
  beast::get_lowest_layer(socket).expires_after(std::chrono::seconds(15));
  beast::get_lowest_layer(socket).connect(resolver.resolve(host, port));
  const auto target = "/v1/program-preview?workspace_id=" + workspace_id +
                      "&relative_path=" + relative_path;
  socket.handshake(host, target);
  std::uint64_t operations = 0;
  bool summary = false;
  for (;;) {
    beast::flat_buffer buffer;
    beast::error_code error;
    socket.read(buffer, error);
    if (error == websocket::error::closed) break;
    assert(!error);
    std::vector<std::uint8_t> bytes(buffer.size());
    asio::buffer_copy(asio::buffer(bytes), buffer.data());
    linuxcnc::v1::ProgramPreviewEvent event;
    assert(event.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    assert(!summary);
    if (event.has_batch()) {
      assert(event.batch().operations_size() > 0);
      assert(static_cast<std::size_t>(event.batch().operations_size()) <=
             batch_limit);
      operations += event.batch().operations_size();
    } else if (event.has_summary()) {
      assert(event.summary().operation_count() == operations);
      assert(event.summary().has_extents());
      summary = true;
    } else if (event.has_error()) {
      std::cerr << "Program preview error: " << event.error().message() << "\n";
      std::abort();
    }
  }
  assert(socket.reason().code == websocket::close_code::normal);
  assert(operations > 0);
  assert(summary);
}

void verify_chunked_preview_stream(linuxcnc::v1::ProgramService::Stub* program,
                                   const std::string& telemetry_endpoint,
                                   std::size_t batch_limit) {
  const auto workspace = upload_program(program, "chunked-preview.ngc",
                                        make_chunked_preview_gcode());
  verify_preview_stream(telemetry_endpoint, workspace.workspace_id(),
                        "chunked-preview.ngc", batch_limit);
  delete_workspace(program, workspace.workspace_id());
}

int probe_reacquire(const std::string& endpoint) {
  const auto channel = make_channel(endpoint);
  auto hal = linuxcnc::v1::HalService::NewStub(channel);
  auto scope = linuxcnc::v1::ScopeService::NewStub(channel);
  (void)get_status_with_retry(
      linuxcnc::v1::MachineService::NewStub(channel).get());

  grpc::ClientContext component_context;
  component_context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds(5));
  auto component = hal->ComponentSession(&component_context);
  linuxcnc::v1::ComponentSessionMessage request;
  request.mutable_open()->set_name("grpc-shutdown-owned");
  request.mutable_open()->set_prefix("grpc-shutdown-owned");
  assert(component->Write(request));
  linuxcnc::v1::ComponentSessionMessage response;
  assert(component->Read(&response) && response.has_metadata());
  request.Clear();
  request.mutable_pin()->set_name("value");
  request.mutable_pin()->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  request.mutable_pin()->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  assert(component->Write(request));
  request.Clear();
  request.mutable_ready()->set_ready(true);
  assert(component->Write(request));
  do {
    assert(component->Read(&response));
  } while (!response.has_metadata());
  assert(response.metadata().ready());

  grpc::ClientContext scope_context;
  scope_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(5));
  linuxcnc::v1::ScopeControlState scope_response;
  assert(scope->GetStatus(&scope_context, {}, &scope_response).ok());
  assert(!scope_response.websocket_path().empty());
  grpc::ClientContext stop_context;
  assert(scope->Stop(&stop_context, {}, &scope_response).ok());
  request.Clear();
  request.mutable_close();
  assert(component->Write(request));
  component->WritesDone();
  while (component->Read(&response)) {
  }
  const auto component_status = component->Finish();
  if (!component_status.ok()) {
    std::cerr << "ComponentSession reacquire failed with status "
              << component_status.error_code() << ": "
              << component_status.error_message() << "\n";
    return 1;
  }
  std::cout << "LIVE_REACQUIRE_READY\n" << std::flush;
  return 0;
}

int hold_shutdown(const std::string& endpoint) {
  const auto channel = make_channel(endpoint);
  auto machine = linuxcnc::v1::MachineService::NewStub(channel);
  auto program = linuxcnc::v1::ProgramService::NewStub(channel);
  auto hal = linuxcnc::v1::HalService::NewStub(channel);
  (void)get_status_with_retry(machine.get());

  grpc::ClientContext error_context;
  error_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(15));
  auto errors = machine->WatchErrors(&error_context, {});

  grpc::ClientContext upload_context;
  upload_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(15));
  linuxcnc::v1::UploadWorkspaceResponse upload_response;
  auto upload = program->UploadWorkspace(&upload_context, &upload_response);
  linuxcnc::v1::UploadWorkspaceRequest upload_request;
  upload_request.set_archive_chunk(
      workspace_archive_fixture("partial.ngc", "G1 X1\n"));
  assert(upload->Write(upload_request));

  grpc::ClientContext component_context;
  component_context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds(15));
  auto component = hal->ComponentSession(&component_context);
  linuxcnc::v1::ComponentSessionMessage component_request;
  component_request.mutable_open()->set_name("grpc-shutdown-owned");
  component_request.mutable_open()->set_prefix("grpc-shutdown-owned");
  assert(component->Write(component_request));
  linuxcnc::v1::ComponentSessionMessage component_response;
  assert(component->Read(&component_response) &&
         component_response.has_metadata());
  component_request.Clear();
  component_request.mutable_pin()->set_name("value");
  component_request.mutable_pin()->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  component_request.mutable_pin()->set_direction(
      linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  assert(component->Write(component_request));
  component_request.Clear();
  component_request.mutable_ready()->set_ready(true);
  assert(component->Write(component_request));
  do {
    assert(component->Read(&component_response));
  } while (!component_response.has_metadata());
  assert(component_response.metadata().ready());

  // Start after the owned component is fully visible so the held watch has no
  // pending mutation to encode before the daemon-shutdown race begins.
  grpc::ClientContext topology_snapshot_context;
  linuxcnc::v1::GetHalTopologyResponse topology_snapshot;
  assert(hal->GetTopology(&topology_snapshot_context, {}, &topology_snapshot)
             .ok());
  grpc::ClientContext topology_context;
  topology_context.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::seconds(15));
  linuxcnc::v1::WatchHalTopologyRequest topology_request;
  topology_request.set_after_sequence(topology_snapshot.sequence());
  auto topology = hal->WatchTopology(&topology_context, topology_request);

  std::cout << "LIVE_SHUTDOWN_READY\n" << std::flush;

  linuxcnc::v1::LinuxCNCError error;
  while (errors->Read(&error)) {
  }
  require_shutdown_status(errors->Finish(), "WatchErrors");
  linuxcnc::v1::WatchHalTopologyEvent topology_event;
  while (topology->Read(&topology_event)) {
  }
  require_shutdown_status(topology->Finish(), "WatchTopology");
  upload->WritesDone();
  const auto upload_status = upload->Finish();
  if (upload_status.ok()) {
    // The archive chunk is complete, so a graceful shutdown may let the
    // upload commit before the stream cancellation reaches this RPC.
    assert(!upload_response.workspace_id().empty());
  } else {
    require_shutdown_status(upload_status, "UploadWorkspace");
  }
  component->WritesDone();
  while (component->Read(&component_response)) {
  }
  require_shutdown_status(component->Finish(), "ComponentSession");
  std::cout << "LIVE_SHUTDOWN_TERMINATED\n" << std::flush;
  return 0;
}

}  // namespace

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

  auto hal = linuxcnc::v1::HalService::NewStub(channel);
  grpc::ClientContext topology_context;
  linuxcnc::v1::GetHalTopologyResponse topology;
  const auto topology_status =
      hal->GetTopology(&topology_context, {}, &topology);
  if (!topology_status.ok()) {
    std::cerr << "HAL topology failed: " << topology_status.error_message()
              << "\n";
    return 1;
  }
  assert(topology.sequence() != 0);
  assert(topology.has_topology());
  assert(topology.topology().pins_size() > 0);

  verify_hal_value_subscription_lifecycle(hal.get(), telemetry_endpoint,
                                          topology.topology().pins(0).name());

  grpc::ClientContext future_topology_context;
  future_topology_context.set_deadline(std::chrono::system_clock::now() +
                                       std::chrono::seconds(2));
  linuxcnc::v1::WatchHalTopologyRequest future_topology_request;
  future_topology_request.set_after_sequence(topology.sequence() + 1000);
  auto future_topology =
      hal->WatchTopology(&future_topology_context, future_topology_request);
  linuxcnc::v1::WatchHalTopologyEvent future_topology_event;
  assert(future_topology->Read(&future_topology_event));
  assert(future_topology_event.sequence() <
         future_topology_request.after_sequence());
  assert(future_topology_event.has_topology());
  future_topology_context.TryCancel();
  (void)future_topology->Finish();

  // A real HAL mutation must advance the typed topology stream.
  grpc::ClientContext topology_watch_context;
  topology_watch_context.set_deadline(std::chrono::system_clock::now() +
                                      std::chrono::seconds(5));
  linuxcnc::v1::WatchHalTopologyRequest topology_watch_request;
  topology_watch_request.set_after_sequence(topology.sequence());
  auto topology_watch =
      hal->WatchTopology(&topology_watch_context, topology_watch_request);
  linuxcnc::v1::CreateHalSignalRequest watched_signal_request;
  watched_signal_request.set_name("grpc-live-topology-watch");
  watched_signal_request.set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  linuxcnc::v1::CreateHalSignalResponse watched_signal_response;
  grpc::ClientContext watched_signal_context;
  assert(hal->CreateSignal(&watched_signal_context, watched_signal_request,
                           &watched_signal_response)
             .ok());
  linuxcnc::v1::WatchHalTopologyEvent topology_event;
  bool saw_watched_signal = false;
  while (topology_watch->Read(&topology_event)) {
    if (topology_event.sequence() <= topology.sequence()) continue;
    for (const auto& signal : topology_event.topology().signals()) {
      if (signal.name() == watched_signal_request.name()) {
        saw_watched_signal = true;
        break;
      }
    }
    if (saw_watched_signal) break;
  }
  topology_watch_context.TryCancel();
  (void)topology_watch->Finish();
  assert(saw_watched_signal);

  // Every legally admitted watcher must receive the same immutable snapshot;
  // fan-out must not consume one HAL worker queue slot per stream.
  constexpr int topology_fanout = 128;
  std::vector<std::unique_ptr<grpc::ClientContext>> fanout_contexts;
  std::vector<
      std::unique_ptr<grpc::ClientReader<linuxcnc::v1::WatchHalTopologyEvent>>>
      fanout_streams;
  fanout_contexts.reserve(topology_fanout);
  fanout_streams.reserve(topology_fanout);
  linuxcnc::v1::WatchHalTopologyRequest fanout_request;
  fanout_request.set_after_sequence(topology_event.sequence());
  for (int index = 0; index < topology_fanout; ++index) {
    auto context = std::make_unique<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() +
                          std::chrono::seconds(8));
    fanout_streams.push_back(hal->WatchTopology(context.get(), fanout_request));
    fanout_contexts.push_back(std::move(context));
  }
  linuxcnc::v1::CreateHalSignalRequest fanout_signal_request;
  fanout_signal_request.set_name("grpc-live-topology-fanout");
  fanout_signal_request.set_type(linuxcnc::v1::HAL_TYPE_BIT);
  linuxcnc::v1::CreateHalSignalResponse fanout_signal_response;
  grpc::ClientContext fanout_signal_context;
  assert(hal->CreateSignal(&fanout_signal_context, fanout_signal_request,
                           &fanout_signal_response)
             .ok());
  std::uint64_t fanout_sequence = 0;
  for (auto& stream : fanout_streams) {
    linuxcnc::v1::WatchHalTopologyEvent event;
    do {
      assert(stream->Read(&event));
    } while (event.sequence() <= fanout_request.after_sequence());
    assert(event.sequence() > fanout_request.after_sequence());
    if (fanout_sequence == 0) fanout_sequence = event.sequence();
    assert(event.sequence() == fanout_sequence);
  }
  for (std::size_t index = 0; index < fanout_streams.size(); ++index) {
    fanout_contexts[index]->TryCancel();
    (void)fanout_streams[index]->Finish();
  }

  linuxcnc::v1::GetHalWriterMetadataResponse writer_metadata;
  grpc::ClientContext writer_metadata_context;
  assert(hal->GetWriterMetadata(&writer_metadata_context, {}, &writer_metadata)
             .ok());
  assert(writer_metadata.metadata().writer_id() == "linuxcnc-grpc-server");
  assert(writer_metadata.metadata().ready());
  linuxcnc::v1::SetHalWriterReadyRequest writer_ready;
  writer_ready.set_ready(false);
  grpc::ClientContext writer_ready_context;
  assert(hal->SetWriterReady(&writer_ready_context, writer_ready, &empty).ok());
  linuxcnc::v1::GetHalWriterMetadataResponse unready_metadata;
  grpc::ClientContext unready_metadata_context;
  assert(
      hal->GetWriterMetadata(&unready_metadata_context, {}, &unready_metadata)
          .ok());
  assert(!unready_metadata.metadata().ready());
  writer_ready.set_ready(true);
  grpc::ClientContext restore_writer_ready_context;
  assert(
      hal->SetWriterReady(&restore_writer_ready_context, writer_ready, &empty)
          .ok());
  linuxcnc::v1::GetHalWriterMetadataResponse restored_metadata;
  grpc::ClientContext restored_metadata_context;
  assert(
      hal->GetWriterMetadata(&restored_metadata_context, {}, &restored_metadata)
          .ok());
  assert(restored_metadata.metadata().ready());

  const auto& pin = topology.topology().pins(0);
  linuxcnc::v1::HalReadRequest read_request;
  auto* item = read_request.add_items();
  item->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  item->set_name(pin.name());
  grpc::ClientContext read_context;
  linuxcnc::v1::HalReadResponse read_response;
  const auto read_status =
      hal->Read(&read_context, read_request, &read_response);
  if (!read_status.ok()) {
    std::cerr << "HAL read failed for " << pin.name() << ": "
              << read_status.error_message() << "\n";
    return 1;
  }
  assert(read_response.values_size() == 1);
  assert(read_response.values(0).value().type() !=
         linuxcnc::v1::HAL_TYPE_UNSPECIFIED);

  // Prove that both 64-bit HAL integer types survive the real HAL and
  // protobuf boundaries without a JavaScript-number-style precision loss.
  const std::int64_t signed_value = -9007199254740993LL;
  const std::uint64_t unsigned_value = 18446744073709551600ULL;
  for (const auto& [name, type] :
       {std::pair{"grpc-live-s64", linuxcnc::v1::HAL_TYPE_S64},
        std::pair{"grpc-live-u64", linuxcnc::v1::HAL_TYPE_U64}}) {
    linuxcnc::v1::CreateHalSignalRequest create_signal_request;
    create_signal_request.set_name(name);
    create_signal_request.set_type(type);
    linuxcnc::v1::CreateHalSignalResponse create_signal_response;
    grpc::ClientContext create_signal_context;
    const auto create_signal_status = hal->CreateSignal(
        &create_signal_context, create_signal_request, &create_signal_response);
    assert(create_signal_status.ok());
    assert(create_signal_response.signal().name() == name);
    assert(create_signal_response.signal().type() == type);
  }

  linuxcnc::v1::CreateHalSignalRequest conflicting_signal;
  conflicting_signal.set_name("grpc-live-s64");
  conflicting_signal.set_type(linuxcnc::v1::HAL_TYPE_U64);
  linuxcnc::v1::CreateHalSignalResponse conflicting_signal_response;
  grpc::ClientContext conflicting_signal_context;
  const auto conflicting_signal_status =
      hal->CreateSignal(&conflicting_signal_context, conflicting_signal,
                        &conflicting_signal_response);
  assert(conflicting_signal_status.error_code() ==
         grpc::StatusCode::INVALID_ARGUMENT);

  linuxcnc::v1::HalWrite exact_write;
  auto* signed_write = exact_write.add_writes();
  signed_write->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  signed_write->mutable_item()->set_name("grpc-live-s64");
  signed_write->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  signed_write->mutable_value()->set_s64(signed_value);
  auto* unsigned_write = exact_write.add_writes();
  unsigned_write->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  unsigned_write->mutable_item()->set_name("grpc-live-u64");
  unsigned_write->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_U64);
  unsigned_write->mutable_value()->set_u64(unsigned_value);
  linuxcnc::v1::HalWriteResponse exact_write_response;
  grpc::ClientContext exact_write_context;
  const auto exact_write_status =
      hal->Write(&exact_write_context, exact_write, &exact_write_response);
  assert(exact_write_status.ok());
  assert(exact_write_response.values_size() == 2);
  assert(exact_write_response.values(0).value().s64() == signed_value);
  assert(exact_write_response.values(1).value().u64() == unsigned_value);

  linuxcnc::v1::HalWrite invalid_batch;
  auto* valid_batch_write = invalid_batch.add_writes();
  valid_batch_write->mutable_item()->set_kind(
      linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  valid_batch_write->mutable_item()->set_name("grpc-live-s64");
  valid_batch_write->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  valid_batch_write->mutable_value()->set_s64(signed_value + 1);
  auto* invalid_batch_write = invalid_batch.add_writes();
  invalid_batch_write->mutable_item()->set_kind(
      linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  invalid_batch_write->mutable_item()->set_name("grpc-live-missing");
  invalid_batch_write->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  invalid_batch_write->mutable_value()->set_s64(0);
  linuxcnc::v1::HalWriteResponse invalid_batch_response;
  grpc::ClientContext invalid_batch_context;
  const auto invalid_batch_status = hal->Write(
      &invalid_batch_context, invalid_batch, &invalid_batch_response);
  assert(invalid_batch_status.error_code() ==
         grpc::StatusCode::FAILED_PRECONDITION);

  linuxcnc::v1::HalReadRequest exact_read;
  auto* signed_read = exact_read.add_items();
  signed_read->set_kind(linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  signed_read->set_name("grpc-live-s64");
  auto* unsigned_read = exact_read.add_items();
  unsigned_read->set_kind(linuxcnc::v1::HAL_ITEM_KIND_SIGNAL);
  unsigned_read->set_name("grpc-live-u64");
  linuxcnc::v1::HalReadResponse exact_read_response;
  grpc::ClientContext exact_read_context;
  const auto exact_read_status =
      hal->Read(&exact_read_context, exact_read, &exact_read_response);
  assert(exact_read_status.ok());
  assert(exact_read_response.values_size() == 2);
  assert(exact_read_response.values(0).value().s64() == signed_value);
  assert(exact_read_response.values(1).value().u64() == unsigned_value);

  // A disconnected client-owned component must disappear with all its pins.
  grpc::ClientContext component_context;
  component_context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds(5));
  auto component = hal->ComponentSession(&component_context);
  linuxcnc::v1::ComponentSessionMessage component_open;
  component_open.mutable_open()->set_name("grpc-live-component");
  component_open.mutable_open()->set_prefix("grpc-live-component");
  assert(component->Write(component_open));
  linuxcnc::v1::ComponentSessionMessage component_response;
  assert(component->Read(&component_response));
  assert(component_response.has_metadata());
  assert(component_response.metadata().writer_id() == "grpc-live-component");
  assert(!component_response.metadata().ready());

  linuxcnc::v1::ComponentSessionMessage component_pin;
  component_pin.mutable_pin()->set_name("value");
  component_pin.mutable_pin()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  component_pin.mutable_pin()->set_direction(
      linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  assert(component->Write(component_pin));
  linuxcnc::v1::ComponentSessionMessage component_ready;
  component_ready.mutable_ready()->set_ready(true);
  assert(component->Write(component_ready));
  assert(component->Read(&component_response));
  assert(component_response.has_metadata());
  assert(component_response.metadata().ready());

  linuxcnc::v1::ComponentSessionMessage component_value;
  component_value.mutable_value()->mutable_item()->set_kind(
      linuxcnc::v1::HAL_ITEM_KIND_PIN);
  component_value.mutable_value()->mutable_item()->set_name(
      "grpc-live-component.value");
  component_value.mutable_value()->mutable_value()->set_type(
      linuxcnc::v1::HAL_TYPE_S64);
  component_value.mutable_value()->mutable_value()->set_s64(signed_value);
  assert(component->Write(component_value));
  bool saw_component_acknowledgement = false;
  bool saw_component_delta = false;
  while (component->Read(&component_response)) {
    if (component_response.has_value()) {
      assert(component_response.value().item().kind() ==
             linuxcnc::v1::HAL_ITEM_KIND_PIN);
      assert(component_response.value().item().name() ==
             "grpc-live-component.value");
      assert(component_response.value().value().type() ==
             linuxcnc::v1::HAL_TYPE_S64);
      assert(component_response.value().value().s64() == signed_value);
      saw_component_acknowledgement = true;
    }
    if (component_response.has_delta()) {
      for (const auto& value : component_response.delta().values()) {
        if (value.item().name() == "grpc-live-component.value") {
          assert(value.value().s64() == signed_value);
          saw_component_delta = true;
        }
      }
    }
    if (saw_component_acknowledgement && saw_component_delta) break;
  }
  assert(saw_component_acknowledgement);
  assert(saw_component_delta);
  linuxcnc::v1::ComponentSessionMessage component_close;
  component_close.mutable_close();
  assert(component->Write(component_close));
  component->WritesDone();
  const auto component_status = component->Finish();
  assert(component_status.ok());

  grpc::ClientContext cleanup_topology_context;
  linuxcnc::v1::GetHalTopologyResponse cleanup_topology;
  const auto cleanup_topology_status =
      hal->GetTopology(&cleanup_topology_context, {}, &cleanup_topology);
  assert(cleanup_topology_status.ok());
  for (const auto& item : cleanup_topology.topology().components()) {
    assert(item.name() != "grpc-live-component");
  }
  for (const auto& item : cleanup_topology.topology().pins()) {
    assert(item.name() != "grpc-live-component.value");
  }

  // Cancellation, rather than an explicit Close message, owns the same HAL
  // cleanup path.
  grpc::ClientContext abrupt_context;
  abrupt_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(5));
  auto abrupt = hal->ComponentSession(&abrupt_context);
  linuxcnc::v1::ComponentSessionMessage abrupt_open;
  abrupt_open.mutable_open()->set_name("grpc-live-abrupt");
  abrupt_open.mutable_open()->set_prefix("grpc-live-abrupt");
  assert(abrupt->Write(abrupt_open));
  assert(abrupt->Read(&component_response));
  abrupt_context.TryCancel();
  abrupt->WritesDone();
  const auto abrupt_status = abrupt->Finish();
  assert(abrupt_status.error_code() == grpc::StatusCode::CANCELLED);
  bool abrupt_removed = false;
  for (int attempt = 0; attempt < 20 && !abrupt_removed; ++attempt) {
    grpc::ClientContext context;
    linuxcnc::v1::GetHalTopologyResponse current;
    assert(hal->GetTopology(&context, {}, &current).ok());
    abrupt_removed = true;
    for (const auto& item : current.topology().components()) {
      if (item.name() == "grpc-live-abrupt") abrupt_removed = false;
    }
    if (!abrupt_removed)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  assert(abrupt_removed);

  // Scope controls use the shared gRPC control plane while capture data uses
  // the shared WebSocket telemetry listener.
  auto scope = linuxcnc::v1::ScopeService::NewStub(channel);
  grpc::ClientContext scope_context;
  scope_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(10));
  linuxcnc::v1::ScopeControlState scope_response;
  assert(scope->GetStatus(&scope_context, {}, &scope_response).ok());
  assert(scope_response.has_status());
  assert(!scope_response.websocket_path().empty());

  const linuxcnc::v1::HalPinInfo* scope_pin = nullptr;
  for (const auto& candidate : topology.topology().pins()) {
    if (candidate.type() == linuxcnc::v1::HAL_TYPE_BIT ||
        candidate.type() == linuxcnc::v1::HAL_TYPE_FLOAT ||
        candidate.type() == linuxcnc::v1::HAL_TYPE_S32 ||
        candidate.type() == linuxcnc::v1::HAL_TYPE_U32) {
      scope_pin = &candidate;
      break;
    }
  }
  assert(scope_pin != nullptr);
  linuxcnc::v1::ScopeConfigure scope_configure;
  auto* acquisition = scope_configure.mutable_config();
  acquisition->set_thread_name("servo-thread");
  acquisition->set_multiplier(1);
  acquisition->set_automatic(true);
  auto* scope_channel = acquisition->add_channels();
  scope_channel->set_index(0);
  scope_channel->set_enabled(true);
  scope_channel->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  scope_channel->mutable_item()->set_name(scope_pin->name());
  grpc::ClientContext configure_context;
  assert(scope->Configure(&configure_context, scope_configure, &scope_response)
             .ok());

  const auto [scope_host, scope_port] = split_endpoint(telemetry_endpoint);
  asio::io_context scope_io;
  tcp::resolver scope_resolver(scope_io);
  websocket::stream<beast::tcp_stream> scope_socket(scope_io);
  beast::get_lowest_layer(scope_socket).expires_after(std::chrono::seconds(5));
  beast::get_lowest_layer(scope_socket)
      .connect(scope_resolver.resolve(scope_host, scope_port));
  scope_socket.handshake(scope_host, scope_response.websocket_path());

  linuxcnc::v1::ScopeRun scope_run;
  scope_run.set_mode(linuxcnc::v1::SCOPE_RUN_MODE_ROLL);
  grpc::ClientContext run_context;
  assert(scope->Run(&run_context, scope_run, &scope_response).ok());

  beast::flat_buffer scope_buffer;
  scope_socket.read(scope_buffer);
  std::vector<std::uint8_t> scope_bytes(scope_buffer.size());
  asio::buffer_copy(asio::buffer(scope_bytes), scope_buffer.data());
  linuxcnc::v1::ScopeTelemetryFrame scope_frame;
  assert(scope_frame.ParseFromArray(scope_bytes.data(),
                                    static_cast<int>(scope_bytes.size())));
  assert(scope_frame.has_capture() || scope_frame.has_roll());

  grpc::ClientContext trigger_context;
  assert(scope->Trigger(&trigger_context, {}, &scope_response).ok());

  grpc::ClientContext stop_context;
  assert(scope->Stop(&stop_context, {}, &scope_response).ok());

  for (const auto mode : {linuxcnc::v1::SCOPE_RUN_MODE_SINGLE,
                          linuxcnc::v1::SCOPE_RUN_MODE_RUN}) {
    grpc::ClientContext mode_configure_context;
    assert(scope
               ->Configure(&mode_configure_context, scope_configure,
                           &scope_response)
               .ok());
    linuxcnc::v1::ScopeRun mode_request;
    mode_request.set_mode(mode);
    grpc::ClientContext mode_run_context;
    assert(scope->Run(&mode_run_context, mode_request, &scope_response).ok());
    grpc::ClientContext mode_stop_context;
    assert(scope->Stop(&mode_stop_context, {}, &scope_response).ok());
  }
  scope_socket.close(websocket::close_code::normal);

  // Saturation deliberately pressures LinuxCNC's command channel, so keep it
  // last while still exercising the real RPC admission boundary.
  verify_priority_admission(machine.get());

  std::cout << "native LinuxCNC gRPC live integration passed\n";
  return 0;
}

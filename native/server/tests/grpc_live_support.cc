#include "grpc_live_support.hpp"

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

namespace grpc_live {

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

}  // namespace grpc_live

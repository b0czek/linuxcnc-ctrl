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

namespace grpc_live {
void require_shutdown_status(const grpc::Status& status, const char* name) {
  if (status.error_code() != grpc::StatusCode::UNAVAILABLE &&
      status.error_code() != grpc::StatusCode::CANCELLED) {
    std::cerr << name << " ended with unexpected status " << status.error_code()
              << ": " << status.error_message() << "\n";
    std::abort();
  }
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
  auto component = hal->RunComponent(&component_context);
  linuxcnc::v1::HalComponentClientMessage request;
  auto* create = request.mutable_create();
  create->set_name("grpc-shutdown-owned");
  create->set_prefix("grpc-shutdown-owned");
  auto* pin = create->add_pins();
  pin->set_name("value");
  pin->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  pin->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  assert(component->Write(request));
  linuxcnc::v1::HalComponentServerMessage response;
  assert(component->Read(&response) && response.has_attached());
  const auto generation = response.attached().generation();
  request.Clear();
  auto* activate = request.mutable_activate();
  activate->set_generation(generation);
  auto* value = activate->add_values();
  value->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  value->mutable_item()->set_name("grpc-shutdown-owned.value");
  value->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  value->mutable_value()->set_float_value(0.0);
  assert(component->Write(request));
  assert(component->Read(&response) && response.has_active());

  grpc::ClientContext scope_context;
  scope_context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(5));
  linuxcnc::v1::ScopeControlState scope_response;
  assert(scope->GetStatus(&scope_context, {}, &scope_response).ok());
  assert(!scope_response.websocket_path().empty());
  grpc::ClientContext stop_context;
  assert(scope->Stop(&stop_context, {}, &scope_response).ok());
  request.Clear();
  request.mutable_close()->set_generation(generation);
  request.mutable_close()->set_mode(
      linuxcnc::v1::HAL_COMPONENT_CLOSE_MODE_DESTROY);
  assert(component->Write(request));
  component->WritesDone();
  while (component->Read(&response)) {
  }
  const auto component_status = component->Finish();
  if (!component_status.ok()) {
    std::cerr << "RunComponent reacquire failed with status "
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
  auto component = hal->RunComponent(&component_context);
  linuxcnc::v1::HalComponentClientMessage component_request;
  auto* create = component_request.mutable_create();
  create->set_name("grpc-shutdown-owned");
  create->set_prefix("grpc-shutdown-owned");
  auto* pin = create->add_pins();
  pin->set_name("value");
  pin->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  pin->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  assert(component->Write(component_request));
  linuxcnc::v1::HalComponentServerMessage component_response;
  assert(component->Read(&component_response) &&
         component_response.has_attached());
  const auto generation = component_response.attached().generation();
  component_request.Clear();
  auto* activate = component_request.mutable_activate();
  activate->set_generation(generation);
  auto* value = activate->add_values();
  value->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  value->mutable_item()->set_name("grpc-shutdown-owned.value");
  value->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
  value->mutable_value()->set_float_value(0.0);
  assert(component->Write(component_request));
  assert(component->Read(&component_response) &&
         component_response.has_active());

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
  require_shutdown_status(component->Finish(), "RunComponent");
  std::cout << "LIVE_SHUTDOWN_TERMINATED\n" << std::flush;
  return 0;
}

}  // namespace grpc_live

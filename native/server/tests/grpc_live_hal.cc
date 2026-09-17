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

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

bool verify_hal_and_scope(const std::shared_ptr<grpc::Channel>& channel,
                          const std::string& telemetry_endpoint) {
  google::protobuf::Empty empty;
  auto hal = linuxcnc::v1::HalService::NewStub(channel);
  grpc::ClientContext topology_context;
  linuxcnc::v1::GetHalTopologyResponse topology;
  const auto topology_status =
      hal->GetTopology(&topology_context, {}, &topology);
  if (!topology_status.ok()) {
    std::cerr << "HAL topology failed: " << topology_status.error_message()
              << "\n";
    return false;
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
    return false;
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

  // A proxy is created atomically, activated with complete client-owned state,
  // and destroyed only by authenticated explicit close.
  grpc::ClientContext component_context;
  component_context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::seconds(5));
  auto component = hal->RunComponent(&component_context);
  linuxcnc::v1::HalComponentClientMessage component_open;
  auto* component_create = component_open.mutable_create();
  component_create->set_name("grpc-live-component");
  component_create->set_prefix("grpc-live-component");
  auto* component_pin = component_create->add_pins();
  component_pin->set_name("value");
  component_pin->set_type(linuxcnc::v1::HAL_TYPE_S64);
  component_pin->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  auto* component_input = component_create->add_pins();
  component_input->set_name("input");
  component_input->set_type(linuxcnc::v1::HAL_TYPE_S32);
  component_input->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_IN);
  assert(component->Write(component_open));
  linuxcnc::v1::HalComponentServerMessage component_response;
  assert(component->Read(&component_response));
  assert(component_response.has_attached());
  assert(component_response.attached().name() == "grpc-live-component");
  assert(component_response.attached().ownership_token().size() == 32);
  bool initially_disconnected = false;
  for (const auto& retained : component_response.attached().retained_values()) {
    if (retained.item().name() == "grpc-live-component.online") {
      assert(!retained.value().bit());
      initially_disconnected = true;
    }
  }
  assert(initially_disconnected);
  const auto component_generation = component_response.attached().generation();
  const auto component_proxy_id = component_response.attached().proxy_id();

  linuxcnc::v1::HalComponentClientMessage component_ready;
  auto* activation = component_ready.mutable_activate();
  activation->set_generation(component_generation);
  auto* initial = activation->add_values();
  initial->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  initial->mutable_item()->set_name("grpc-live-component.value");
  initial->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  initial->mutable_value()->set_s64(0);
  assert(component->Write(component_ready));
  assert(component->Read(&component_response));
  assert(component_response.has_active());

  linuxcnc::v1::HalReadRequest connected_read;
  auto* connected_ref = connected_read.add_items();
  connected_ref->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  connected_ref->set_name("grpc-live-component.online");
  linuxcnc::v1::HalReadResponse connected_response;
  grpc::ClientContext connected_context;
  assert(
      hal->Read(&connected_context, connected_read, &connected_response).ok());
  assert(connected_response.values_size() == 1);
  assert(connected_response.values(0).value().bit());

  linuxcnc::v1::HalComponentClientMessage component_heartbeat;
  component_heartbeat.mutable_heartbeat()->set_generation(component_generation);
  assert(component->Write(component_heartbeat));
  assert(component->Read(&component_response));
  assert(component_response.has_heartbeat_ack());
  assert(component_response.heartbeat_ack().generation() ==
         component_generation);

  linuxcnc::v1::HalComponentClientMessage component_value;
  auto* update = component_value.mutable_update();
  update->set_generation(component_generation);
  update->set_sequence(1);
  auto* update_value = update->add_values();
  update_value->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  update_value->mutable_item()->set_name("grpc-live-component.value");
  update_value->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S64);
  update_value->mutable_value()->set_s64(signed_value);
  assert(component->Write(component_value));
  bool saw_component_acknowledgement = false;
  while (component->Read(&component_response)) {
    if (component_response.has_update_ack()) {
      assert(component_response.update_ack().sequence() == 1);
      saw_component_acknowledgement = true;
    }
    if (saw_component_acknowledgement) break;
  }
  assert(saw_component_acknowledgement);

  linuxcnc::v1::HalWrite input_write;
  auto* input_update = input_write.add_writes();
  input_update->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  input_update->mutable_item()->set_name("grpc-live-component.input");
  input_update->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S32);
  input_update->mutable_value()->set_s32(55);
  linuxcnc::v1::HalWriteResponse input_write_response;
  grpc::ClientContext input_write_context;
  assert(hal->Write(&input_write_context, input_write, &input_write_response)
             .ok());
  bool saw_input_delta = false;
  while (component->Read(&component_response)) {
    if (!component_response.has_delta()) continue;
    assert(component_response.delta().generation() == component_generation);
    for (const auto& value : component_response.delta().values()) {
      if (value.item().name() == "grpc-live-component.input") {
        assert(value.value().s32() == 55);
        saw_input_delta = true;
      }
    }
    if (saw_input_delta) break;
  }
  assert(saw_input_delta);

  linuxcnc::v1::HalComponentClientMessage component_close;
  component_close.mutable_close()->set_generation(component_generation);
  component_close.mutable_close()->set_mode(
      linuxcnc::v1::HAL_COMPONENT_CLOSE_MODE_DESTROY);
  assert(component->Write(component_close));
  component->WritesDone();
  assert(component->Read(&component_response));
  assert(component_response.has_closed());
  assert(component_response.closed().proxy_id() == component_proxy_id);
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
    assert(item.name() != "grpc-live-component.input");
    assert(item.name() != "grpc-live-component.online");
  }

  // Transport loss detaches but retains the proxy and its HAL objects.
  grpc::ClientContext abrupt_context;
  abrupt_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(5));
  auto abrupt = hal->RunComponent(&abrupt_context);
  linuxcnc::v1::HalComponentClientMessage abrupt_open;
  auto* abrupt_create = abrupt_open.mutable_create();
  abrupt_create->set_name("grpc-live-abrupt");
  abrupt_create->set_prefix("grpc-live-abrupt");
  auto* abrupt_pin = abrupt_create->add_pins();
  abrupt_pin->set_name("safe");
  abrupt_pin->set_type(linuxcnc::v1::HAL_TYPE_S32);
  abrupt_pin->set_direction(linuxcnc::v1::HAL_PIN_DIRECTION_OUT);
  abrupt_pin->mutable_disconnect_value()->set_type(linuxcnc::v1::HAL_TYPE_S32);
  abrupt_pin->mutable_disconnect_value()->set_s32(42);
  assert(abrupt->Write(abrupt_open));
  assert(abrupt->Read(&component_response));
  const auto abrupt_id = component_response.attached().proxy_id();
  const auto abrupt_token = component_response.attached().ownership_token();
  const auto abrupt_generation = component_response.attached().generation();

  linuxcnc::v1::HalComponentClientMessage abrupt_activate;
  abrupt_activate.mutable_activate()->set_generation(abrupt_generation);
  auto* abrupt_initial = abrupt_activate.mutable_activate()->add_values();
  abrupt_initial->mutable_item()->set_kind(linuxcnc::v1::HAL_ITEM_KIND_PIN);
  abrupt_initial->mutable_item()->set_name("grpc-live-abrupt.safe");
  abrupt_initial->mutable_value()->set_type(linuxcnc::v1::HAL_TYPE_S32);
  abrupt_initial->mutable_value()->set_s32(7);
  assert(abrupt->Write(abrupt_activate));
  assert(abrupt->Read(&component_response));
  assert(component_response.has_active());

  grpc::ClientContext invalid_attach_context;
  auto invalid_attach = hal->RunComponent(&invalid_attach_context);
  linuxcnc::v1::HalComponentClientMessage invalid_attach_request;
  invalid_attach_request.mutable_attach()->set_proxy_id(abrupt_id);
  invalid_attach_request.mutable_attach()->set_name("grpc-live-abrupt");
  invalid_attach_request.mutable_attach()->set_ownership_token(
      std::string(32, 'x'));
  assert(invalid_attach->Write(invalid_attach_request));
  invalid_attach->WritesDone();
  assert(!invalid_attach->Read(&component_response));
  assert(invalid_attach->Finish().error_code() ==
         grpc::StatusCode::UNAUTHENTICATED);

  grpc::ClientContext concurrent_attach_context;
  auto concurrent_attach = hal->RunComponent(&concurrent_attach_context);
  linuxcnc::v1::HalComponentClientMessage concurrent_attach_request;
  concurrent_attach_request.mutable_attach()->set_proxy_id(abrupt_id);
  concurrent_attach_request.mutable_attach()->set_name("grpc-live-abrupt");
  concurrent_attach_request.mutable_attach()->set_ownership_token(abrupt_token);
  assert(concurrent_attach->Write(concurrent_attach_request));
  concurrent_attach->WritesDone();
  assert(!concurrent_attach->Read(&component_response));
  assert(concurrent_attach->Finish().error_code() ==
         grpc::StatusCode::ALREADY_EXISTS);

  abrupt_context.TryCancel();
  abrupt->WritesDone();
  const auto abrupt_status = abrupt->Finish();
  assert(abrupt_status.error_code() == grpc::StatusCode::CANCELLED);
  bool abrupt_retained = false;
  for (int attempt = 0; attempt < 20 && !abrupt_retained; ++attempt) {
    grpc::ClientContext context;
    linuxcnc::v1::GetHalTopologyResponse current;
    assert(hal->GetTopology(&context, {}, &current).ok());
    for (const auto& item : current.topology().components()) {
      if (item.name() == "grpc-live-abrupt") abrupt_retained = true;
    }
    if (!abrupt_retained)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  assert(abrupt_retained);

  grpc::ClientContext reattach_context;
  auto reattach = hal->RunComponent(&reattach_context);
  linuxcnc::v1::HalComponentClientMessage attach;
  attach.mutable_attach()->set_proxy_id(abrupt_id);
  attach.mutable_attach()->set_name("grpc-live-abrupt");
  attach.mutable_attach()->set_ownership_token(abrupt_token);
  assert(reattach->Write(attach));
  assert(reattach->Read(&component_response));
  assert(component_response.has_attached());
  assert(component_response.attached().generation() > abrupt_generation);
  bool saw_safe_value = false;
  bool saw_disconnected = false;
  for (const auto& retained : component_response.attached().retained_values()) {
    if (retained.item().name() == "grpc-live-abrupt.safe") {
      assert(retained.value().s32() == 42);
      saw_safe_value = true;
    } else if (retained.item().name() == "grpc-live-abrupt.online") {
      assert(!retained.value().bit());
      saw_disconnected = true;
    }
  }
  assert(saw_safe_value);
  assert(saw_disconnected);
  linuxcnc::v1::HalComponentClientMessage detach_abrupt;
  detach_abrupt.mutable_close()->set_generation(
      component_response.attached().generation());
  detach_abrupt.mutable_close()->set_mode(
      linuxcnc::v1::HAL_COMPONENT_CLOSE_MODE_DETACH);
  assert(reattach->Write(detach_abrupt));
  reattach->WritesDone();
  assert(reattach->Read(&component_response));
  assert(component_response.has_closed());
  assert(reattach->Finish().ok());

  grpc::ClientContext destroy_context;
  auto destroy = hal->RunComponent(&destroy_context);
  linuxcnc::v1::HalComponentClientMessage final_attach;
  final_attach.mutable_attach()->set_proxy_id(abrupt_id);
  final_attach.mutable_attach()->set_name("grpc-live-abrupt");
  final_attach.mutable_attach()->set_ownership_token(abrupt_token);
  assert(destroy->Write(final_attach));
  assert(destroy->Read(&component_response));
  assert(component_response.has_attached());
  linuxcnc::v1::HalComponentClientMessage destroy_abrupt;
  destroy_abrupt.mutable_close()->set_generation(
      component_response.attached().generation());
  destroy_abrupt.mutable_close()->set_mode(
      linuxcnc::v1::HAL_COMPONENT_CLOSE_MODE_DESTROY);
  assert(destroy->Write(destroy_abrupt));
  destroy->WritesDone();
  assert(destroy->Read(&component_response));
  assert(component_response.has_closed());
  assert(destroy->Finish().ok());

  grpc::ClientContext timeout_context;
  timeout_context.set_deadline(std::chrono::system_clock::now() +
                               std::chrono::seconds(6));
  auto timeout_component = hal->RunComponent(&timeout_context);
  linuxcnc::v1::HalComponentClientMessage timeout_create;
  timeout_create.mutable_create()->set_name("grpc-live-timeout");
  timeout_create.mutable_create()->set_prefix("grpc-live-timeout");
  assert(timeout_component->Write(timeout_create));
  assert(timeout_component->Read(&component_response));
  const auto timeout_id = component_response.attached().proxy_id();
  const auto timeout_token = component_response.attached().ownership_token();
  while (timeout_component->Read(&component_response)) {
  }
  assert(timeout_component->Finish().error_code() ==
         grpc::StatusCode::DEADLINE_EXCEEDED);

  grpc::ClientContext timeout_destroy_context;
  auto timeout_destroy = hal->RunComponent(&timeout_destroy_context);
  linuxcnc::v1::HalComponentClientMessage timeout_attach;
  timeout_attach.mutable_attach()->set_proxy_id(timeout_id);
  timeout_attach.mutable_attach()->set_name("grpc-live-timeout");
  timeout_attach.mutable_attach()->set_ownership_token(timeout_token);
  assert(timeout_destroy->Write(timeout_attach));
  assert(timeout_destroy->Read(&component_response));
  linuxcnc::v1::HalComponentClientMessage timeout_close;
  timeout_close.mutable_close()->set_generation(
      component_response.attached().generation());
  timeout_close.mutable_close()->set_mode(
      linuxcnc::v1::HAL_COMPONENT_CLOSE_MODE_DESTROY);
  assert(timeout_destroy->Write(timeout_close));
  timeout_destroy->WritesDone();
  assert(timeout_destroy->Read(&component_response));
  assert(timeout_destroy->Finish().ok());

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
  return true;
}

}  // namespace grpc_live

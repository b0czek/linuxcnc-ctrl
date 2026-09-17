#include <google/protobuf/empty.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "grpc/server/deferred_write_finish.hpp"
#include "grpc/server/service_factories.hpp"
#include "grpc/server/unary_task_reactor.hpp"
#include "hal/grpc/remote_component_rpc.hpp"
#include "linuxcnc/v1/hal.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/hal/adapter.hpp"
#include "linuxcnc_grpc/hal/grpc/mapping.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"

namespace linuxcnc::server::detail {
namespace {

using namespace linuxcnc::v1;

constexpr int kMaxHalBatchItems = 1024;

::grpc::Status Invalid(const std::string& message) {
  return {::grpc::StatusCode::INVALID_ARGUMENT, message};
}

std::string reference_key(const HalAdapterReference& reference) {
  return std::to_string(static_cast<int>(reference.kind)) + ":" +
         reference.name;
}

::grpc::Status hal_error(const HalAdapterError& error) {
  const auto code =
      error.code() == -ENOENT      ? ::grpc::StatusCode::NOT_FOUND
      : error.code() == -EBUSY     ? ::grpc::StatusCode::RESOURCE_EXHAUSTED
      : error.code() == -ECANCELED ? ::grpc::StatusCode::CANCELLED
      : error.code() == -EINVAL    ? ::grpc::StatusCode::INVALID_ARGUMENT
                                   : ::grpc::StatusCode::FAILED_PRECONDITION;
  return {code, error.what()};
}

std::optional<HalAdapterType> decode_hal_type(HalType type) {
  if (type < HAL_TYPE_BIT || type > HAL_TYPE_U64) return std::nullopt;
  return static_cast<HalAdapterType>(static_cast<int>(type) - 1);
}

HalTelemetryReference telemetry_reference(const HalAdapterReference& source) {
  return {static_cast<HalTelemetryItemKind>(static_cast<int>(source.kind) + 1),
          source.name};
}

HalAdapterReference adapter_reference(const HalTelemetryReference& source) {
  switch (source.kind) {
    case HalTelemetryItemKind::Pin:
      return {HalAdapterItemKind::Pin, source.name};
    case HalTelemetryItemKind::Param:
      return {HalAdapterItemKind::Param, source.name};
    case HalTelemetryItemKind::Signal:
      return {HalAdapterItemKind::Signal, source.name};
  }
  throw HalAdapterError("invalid HAL telemetry item kind", -EINVAL);
}

void encode_subscription(const HalTelemetryDescriptor& source,
                         HalValueSubscription* target) {
  target->Clear();
  target->set_subscription_id(source.subscription_id);
  target->set_websocket_path(source.websocket_path);
  target->set_revision(source.revision);
  target->set_sample_period_ms(
      static_cast<std::uint32_t>(source.sample_period.count()));
  for (const auto& binding : source.bindings) {
    auto* slot = target->add_slots();
    slot->set_slot(binding.slot);
    slot->mutable_item()->set_kind(
        static_cast<HalItemKind>(static_cast<int>(binding.item.kind)));
    slot->mutable_item()->set_name(binding.item.name);
    slot->set_type(static_cast<HalType>(static_cast<int>(binding.type)));
  }
}

class HalServiceImpl final : public HalService::CallbackService,
                             public ManagedGrpcService {
  class TopologyReactor;
  struct TopologySnapshot {
    std::uint64_t sequence = 0;
    linuxcnc::v1::HalTopology topology;
  };

 public:
  ::grpc::Service* service() noexcept override { return this; }

  HalServiceImpl(const DaemonConfig& config, BoundedExecutor& worker,
                 AdmissionCounter& component_admission,
                 AdmissionCounter& stream_admission,
                 std::shared_ptr<HalValueTelemetry> telemetry)
      : adapter_("linuxcnc-grpc", config.max_remote_hal_items),
        worker_(worker),
        stream_admission_(stream_admission),
        telemetry_(std::move(telemetry)),
        topology_period_(config.topology_period),
        remote_components_(adapter_, worker_, component_admission,
                           stream_admission_, callbacks_, config) {
    refresh_topology_snapshot();
    timer_ = std::thread([this] { timer_loop(); });
  }

  ~HalServiceImpl() override { shutdown(); }

  void shutdown() override {
    if (stopping_.exchange(true)) return;
    topology_wakes_.close();
    callbacks_.shutdown();
    timer_condition_.notify_all();
    if (timer_.joinable()) timer_.join();
    auto completion = std::make_shared<std::promise<void>>();
    auto completed = completion->get_future();
    const auto destroy = [this, completion] {
      remote_components_.shutdown_registry();
      completion->set_value();
    };
    if (worker_.submit_cleanup(destroy)) {
      completed.wait();
    } else {
      worker_.drain();
      destroy();
    }
  }

  ::grpc::ServerUnaryReactor* GetTopology(
      ::grpc::CallbackServerContext*, const GetHalTopologyRequest* request,
      GetHalTopologyResponse* response) override {
    return task(request, response, &HalServiceImpl::do_get_topology);
  }

  ::grpc::ServerUnaryReactor* Read(::grpc::CallbackServerContext*,
                                   const HalReadRequest* request,
                                   HalReadResponse* response) override {
    return cancellable_task(request, response, &HalServiceImpl::do_read);
  }

  ::grpc::ServerUnaryReactor* Write(::grpc::CallbackServerContext*,
                                    const HalWrite* request,
                                    HalWriteResponse* response) override {
    return cancellable_task(request, response, &HalServiceImpl::do_write);
  }

  ::grpc::ServerUnaryReactor* CreateValueSubscription(
      ::grpc::CallbackServerContext*,
      const CreateHalValueSubscriptionRequest* request,
      HalValueSubscription* response) override {
    return task(request, response,
                &HalServiceImpl::do_create_value_subscription);
  }

  ::grpc::ServerUnaryReactor* UpdateValueSubscription(
      ::grpc::CallbackServerContext*,
      const UpdateHalValueSubscriptionRequest* request,
      HalValueSubscription* response) override {
    return task(request, response,
                &HalServiceImpl::do_update_value_subscription);
  }

  ::grpc::ServerUnaryReactor* DeleteValueSubscription(
      ::grpc::CallbackServerContext*,
      const DeleteHalValueSubscriptionRequest* request,
      google::protobuf::Empty* response) override {
    return task(request, response,
                &HalServiceImpl::do_delete_value_subscription);
  }

  ::grpc::ServerUnaryReactor* CreateSignal(
      ::grpc::CallbackServerContext*, const CreateHalSignalRequest* request,
      CreateHalSignalResponse* response) override {
    return task(request, response, &HalServiceImpl::do_create_signal);
  }

  ::grpc::ServerUnaryReactor* SetMessageLevel(
      ::grpc::CallbackServerContext*, const SetHalMessageLevelRequest* request,
      google::protobuf::Empty* response) override {
    return task(request, response, &HalServiceImpl::do_set_message_level);
  }

  ::grpc::ServerUnaryReactor* GetWriterMetadata(
      ::grpc::CallbackServerContext*,
      const GetHalWriterMetadataRequest* request,
      GetHalWriterMetadataResponse* response) override {
    return task(request, response, &HalServiceImpl::do_get_writer_metadata);
  }

  ::grpc::ServerUnaryReactor* SetWriterReady(
      ::grpc::CallbackServerContext*, const SetHalWriterReadyRequest* request,
      google::protobuf::Empty* response) override {
    return task(request, response, &HalServiceImpl::do_set_writer_ready);
  }

  ::grpc::Status do_get_topology(const GetHalTopologyRequest*,
                                 GetHalTopologyResponse* response) {
    try {
      const auto snapshot = refresh_topology_snapshot();
      response->set_sequence(snapshot->sequence);
      *response->mutable_topology() = snapshot->topology;
      return ::grpc::Status::OK;
    } catch (const HalAdapterError& error) {
      return hal_error(error);
    }
  }

  ::grpc::ServerWriteReactor<WatchHalTopologyEvent>* WatchTopology(
      ::grpc::CallbackServerContext*,
      const WatchHalTopologyRequest* request) override {
    return new TopologyReactor(*this, request->after_sequence());
  }

  ::grpc::Status do_read(const std::stop_token& token,
                         const HalReadRequest* request,
                         HalReadResponse* response) {
    if (request->items_size() > kMaxHalBatchItems)
      return {::grpc::StatusCode::RESOURCE_EXHAUSTED,
              "HAL read exceeds 1024 items"};
    try {
      std::vector<HalAdapterReference> references;
      std::unordered_set<std::string> unique;
      references.reserve(request->items_size());
      for (const auto& item : request->items()) {
        if (token.stop_requested())
          return {::grpc::StatusCode::CANCELLED, "RPC cancelled"};
        auto decoded = decode_hal_reference(item);
        if (!decoded)
          return Invalid("HAL read contains an invalid item reference");
        if (!unique.insert(reference_key(*decoded)).second)
          return Invalid("HAL read contains a duplicate item reference");
        references.push_back(std::move(*decoded));
      }
      const auto values = adapter_.read_many(references, token);
      for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& value = values[index];
        if (!value) {
          return {::grpc::StatusCode::NOT_FOUND,
                  "HAL item '" + references[index].name + "' was not found"};
        }
        auto* encoded = response->add_values();
        *encoded->mutable_item() = request->items(static_cast<int>(index));
        encode_hal_scalar(value.value(), encoded->mutable_value());
      }
      return ::grpc::Status::OK;
    } catch (const HalAdapterError& error) {
      return hal_error(error);
    }
  }

  ::grpc::Status do_write(const std::stop_token& token, const HalWrite* request,
                          HalWriteResponse* response) {
    if (request->writes_size() > kMaxHalBatchItems)
      return {::grpc::StatusCode::RESOURCE_EXHAUSTED,
              "HAL write exceeds 1024 items"};
    try {
      std::vector<std::pair<HalAdapterReference, HalAdapterValue>> updates;
      std::unordered_set<std::string> unique;
      updates.reserve(request->writes_size());
      for (const auto& write : request->writes()) {
        if (token.stop_requested())
          return {::grpc::StatusCode::CANCELLED, "RPC cancelled"};
        auto reference = decode_hal_reference(write.item());
        auto value = decode_hal_scalar(write.value());
        if (!reference || !value) {
          return Invalid(
              "HAL write contains a mismatched reference or scalar oneof");
        }
        if (!unique.insert(reference_key(*reference)).second)
          return Invalid("HAL write contains a duplicate item reference");
        updates.emplace_back(std::move(reference.value()), value.value());
      }
      std::vector<HalAdapterValue> written;
      if (adapter_.write_many(updates, &written, token) != updates.size()) {
        return {::grpc::StatusCode::FAILED_PRECONDITION,
                "one or more HAL items are missing, mistyped, or not writable"};
      }
      for (std::size_t index = 0; index < updates.size(); ++index) {
        auto* encoded = response->add_values();
        *encoded->mutable_item() =
            request->writes(static_cast<int>(index)).item();
        encode_hal_scalar(written[index], encoded->mutable_value());
      }
      return ::grpc::Status::OK;
    } catch (const HalAdapterError& error) {
      return hal_error(error);
    }
  }

  ::grpc::Status do_create_value_subscription(
      const CreateHalValueSubscriptionRequest* request,
      HalValueSubscription* response) {
    if (request->items_size() == 0)
      return Invalid("HAL value subscription requires at least one item");
    auto resolved = resolve_subscription_items(request->items(), nullptr);
    if (!resolved.first.ok()) return resolved.first;
    const auto period = subscription_period(request->sample_period_ms());
    if (!period)
      return Invalid("HAL value sample period must be between 50 and 60000 ms");
    auto created = telemetry_->create(std::move(resolved.second), *period);
    if (!created)
      return {::grpc::StatusCode::RESOURCE_EXHAUSTED,
              "HAL value subscription limit reached"};
    encode_subscription(*created, response);
    return ::grpc::Status::OK;
  }

  ::grpc::Status do_update_value_subscription(
      const UpdateHalValueSubscriptionRequest* request,
      HalValueSubscription* response) {
    const auto current = telemetry_->descriptor(request->subscription_id());
    if (!current)
      return {::grpc::StatusCode::NOT_FOUND,
              "HAL value subscription was not found"};
    if (current->revision != request->expected_revision())
      return {::grpc::StatusCode::ABORTED,
              "HAL value subscription revision does not match"};
    auto resolved = resolve_subscription_items(request->items(), &*current);
    if (!resolved.first.ok()) return resolved.first;
    const auto period = subscription_period(request->sample_period_ms());
    if (!period)
      return Invalid("HAL value sample period must be between 50 and 60000 ms");
    auto updated = telemetry_->update(request->subscription_id(),
                                      request->expected_revision(),
                                      std::move(resolved.second), *period);
    if (!updated)
      return {::grpc::StatusCode::ABORTED,
              "HAL value subscription changed concurrently"};
    encode_subscription(*updated, response);
    return ::grpc::Status::OK;
  }

  ::grpc::Status do_delete_value_subscription(
      const DeleteHalValueSubscriptionRequest* request,
      google::protobuf::Empty*) {
    if (request->subscription_id().empty())
      return Invalid("HAL value subscription id is required");
    telemetry_->erase(request->subscription_id());
    return ::grpc::Status::OK;
  }

  ::grpc::Status do_create_signal(const CreateHalSignalRequest* request,
                                  CreateHalSignalResponse* response) {
    const auto type = decode_hal_type(request->type());
    if (request->name().empty() || !type) {
      return Invalid("signal name and type are required");
    }
    try {
      if (!adapter_.create_signal(request->name(), *type)) {
        return Invalid("HAL signal was rejected");
      }
      const auto topology = adapter_.topology();
      const auto found = std::find_if(
          topology.signals.begin(), topology.signals.end(),
          [&](const auto& signal) { return signal.name == request->name(); });
      if (found == topology.signals.end()) {
        return {::grpc::StatusCode::INTERNAL,
                "created HAL signal is not visible"};
      }
      HalAdapterTopology one;
      one.signals.push_back(*found);
      linuxcnc::v1::HalTopology encoded;
      encode_hal_topology(one, &encoded);
      *response->mutable_signal() = encoded.signals(0);
      return ::grpc::Status::OK;
    } catch (const HalAdapterError& error) {
      return hal_error(error);
    }
  }

  ::grpc::Status do_set_message_level(const SetHalMessageLevelRequest* request,
                                      google::protobuf::Empty*) {
    if (request->level() < RTAPI_MESSAGE_LEVEL_NONE ||
        request->level() > RTAPI_MESSAGE_LEVEL_ALL) {
      return Invalid("HAL message level is required");
    }
    try {
      adapter_.set_message_level(static_cast<int>(request->level()) - 1);
      return ::grpc::Status::OK;
    } catch (const HalAdapterError& error) {
      return hal_error(error);
    }
  }

  ::grpc::Status do_get_writer_metadata(
      const GetHalWriterMetadataRequest*,
      GetHalWriterMetadataResponse* response) {
    response->mutable_metadata()->set_writer_id("linuxcnc-grpc-server");
    response->mutable_metadata()->set_ready(
        writer_ready_.load(std::memory_order_relaxed));
    return ::grpc::Status::OK;
  }

  ::grpc::Status do_set_writer_ready(const SetHalWriterReadyRequest* request,
                                     google::protobuf::Empty*) {
    writer_ready_.store(request->ready(), std::memory_order_relaxed);
    return ::grpc::Status::OK;
  }

  ::grpc::ServerBidiReactor<HalComponentClientMessage,
                            HalComponentServerMessage>*
  RunComponent(::grpc::CallbackServerContext*) override {
    return remote_components_.run();
  }

 private:
  template <typename Request, typename Response>
  ::grpc::ServerUnaryReactor* task(
      const Request* request, Response* response,
      ::grpc::Status (HalServiceImpl::*method)(const Request*, Response*)) {
    auto owned_request = std::make_shared<Request>(*request);
    return new UnaryTaskReactor<Response>(
        worker_, callbacks_, response,
        [this, owned_request = std::move(owned_request), method](
            const std::stop_token&, Response* task_response) {
          return (this->*method)(owned_request.get(), task_response);
        });
  }

  template <typename Request, typename Response>
  ::grpc::ServerUnaryReactor* cancellable_task(
      const Request* request, Response* response,
      ::grpc::Status (HalServiceImpl::*method)(const std::stop_token&,
                                               const Request*, Response*)) {
    auto owned_request = std::make_shared<Request>(*request);
    return new UnaryTaskReactor<Response>(
        worker_, callbacks_, response,
        [this, owned_request = std::move(owned_request), method](
            const std::stop_token& token, Response* task_response) {
          return (this->*method)(token, owned_request.get(), task_response);
        });
  }

  static std::optional<std::chrono::milliseconds> subscription_period(
      std::uint32_t value) {
    if (value == 0) value = 100;
    if (value < 50 || value > 60000) return std::nullopt;
    return std::chrono::milliseconds(value);
  }

  std::pair<::grpc::Status, std::vector<HalTelemetryResolvedItem>>
  resolve_subscription_items(
      const google::protobuf::RepeatedPtrField<HalItemRef>& items,
      const HalTelemetryDescriptor* existing) {
    if (items.size() > 1024)
      return {{::grpc::StatusCode::RESOURCE_EXHAUSTED,
               "HAL value subscription exceeds 1024 items"},
              {}};
    std::vector<HalAdapterReference> references;
    std::unordered_set<std::string> unique;
    for (const auto& item : items) {
      auto decoded = decode_hal_reference(item);
      if (!decoded)
        return {Invalid("HAL value subscription contains an invalid item"), {}};
      const auto key =
          std::to_string(static_cast<int>(decoded->kind)) + ":" + decoded->name;
      if (!unique.insert(key).second)
        return {Invalid("HAL value subscription contains a duplicate item"),
                {}};
      references.push_back(std::move(*decoded));
    }
    const auto values = adapter_.read_many(references);
    std::unordered_map<std::string, HalTelemetryType> prior;
    if (existing)
      for (const auto& binding : existing->bindings)
        prior.emplace(std::to_string(static_cast<int>(binding.item.kind)) +
                          ":" + binding.item.name,
                      binding.type);
    std::vector<HalTelemetryResolvedItem> result;
    for (std::size_t index = 0; index < references.size(); ++index) {
      const auto reference = telemetry_reference(references[index]);
      HalTelemetryType type = HalTelemetryType::Unavailable;
      const auto& value = values[index];
      if (value) {
        type = static_cast<HalTelemetryType>(value->index() + 1);
      } else {
        const auto found =
            prior.find(std::to_string(static_cast<int>(reference.kind)) + ":" +
                       reference.name);
        if (found == prior.end())
          return {{::grpc::StatusCode::NOT_FOUND,
                   "HAL item '" + reference.name + "' was not found"},
                  {}};
        type = found->second;
      }
      result.push_back({reference, type});
    }
    return {::grpc::Status::OK, std::move(result)};
  }

  void sample_telemetry() {
    const auto samples = telemetry_->due(std::chrono::steady_clock::now());
    std::vector<HalAdapterReference> references;
    std::unordered_map<std::string, std::size_t> reference_indexes;
    for (const auto& due : samples)
      for (const auto& binding : due.bindings) {
        const auto key = std::to_string(static_cast<int>(binding.item.kind)) +
                         ":" + binding.item.name;
        if (reference_indexes.emplace(key, references.size()).second)
          references.push_back(adapter_reference(binding.item));
      }
    const auto values = adapter_.read_many(references);
    for (const auto& due : samples) {
      std::vector<std::optional<HalTelemetryValue>> published;
      published.reserve(due.bindings.size());
      for (const auto& binding : due.bindings) {
        const auto key = std::to_string(static_cast<int>(binding.item.kind)) +
                         ":" + binding.item.name;
        const auto& value = values[reference_indexes.at(key)];
        if (!value) {
          published.emplace_back();
          continue;
        }
        if (value->index() + 1 != static_cast<std::size_t>(binding.type)) {
          published.emplace_back();
          continue;
        }
        published.push_back(value);
      }
      telemetry_->publish(due.subscription_id, due.revision,
                          std::move(published));
    }
  }

  class TopologyReactor final
      : public ::grpc::ServerWriteReactor<WatchHalTopologyEvent> {
   public:
    TopologyReactor(HalServiceImpl& service, std::uint64_t after)
        : service_(service),
          after_(0),
          admitted_(service_.stream_admission_.acquire()),
          gate_(std::make_shared<LifetimeGate<TopologyReactor>>(this)) {
      // Cursors are scoped to a daemon epoch, which is not represented on the
      // wire. Establish every stream with a replacement snapshot.
      (void)after;
      const std::weak_ptr<LifetimeGate<TopologyReactor>> weak = gate_;
      registration_ = service_.callbacks_.register_callback([weak] {
        if (auto gate = weak.lock())
          gate->invoke([](TopologyReactor& reactor) { reactor.shutdown(); });
      });
      if (!registration_) {
        shutdown();
        return;
      }
      if (!admitted_) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "stream admission limit reached"});
        return;
      }
      subscription_ = service_.topology_wakes_.subscribe(
          [weak](const std::shared_ptr<const TopologySnapshot>& snapshot) {
            if (auto gate = weak.lock())
              gate->invoke([snapshot](TopologyReactor& reactor) {
                reactor.offer(snapshot);
              });
          });
      offer(service_.cached_topology_snapshot());
    }
    void OnWriteDone(bool ok) override {
      gate_->invoke([ok](TopologyReactor& reactor) {
        reactor.write_finish_.complete_write(ok);
        if (!reactor.finish_if_ready() && ok) {
          reactor.after_ = reactor.writing_sequence_;
          if (reactor.pending_) {
            auto pending = std::move(reactor.pending_);
            reactor.start_write(pending);
          }
        }
      });
    }
    void OnCancel() override {
      gate_->invoke([](TopologyReactor& reactor) {
        reactor.request_finish(
            {::grpc::StatusCode::CANCELLED, "topology stream cancelled"});
      });
    }
    void OnDone() override {
      subscription_.reset();
      gate_->detach();
      registration_.reset();
      if (admitted_) service_.stream_admission_.release();
      delete this;
    }
    void shutdown() {
      subscription_.reset();
      request_finish({::grpc::StatusCode::UNAVAILABLE, "server shutting down"});
    }

   private:
    void offer(std::shared_ptr<const TopologySnapshot> snapshot) {
      if (!snapshot || snapshot->sequence == after_ ||
          write_finish_.termination_requested() ||
          gate_->state() != LifetimeGate<TopologyReactor>::State::Open)
        return;
      if (write_finish_.write_in_flight()) {
        if (snapshot->sequence > writing_sequence_ &&
            (!pending_ || snapshot->sequence > pending_->sequence))
          pending_ = std::move(snapshot);
        return;
      }
      start_write(snapshot);
    }
    void start_write(const std::shared_ptr<const TopologySnapshot>& snapshot) {
      if (!snapshot || snapshot->sequence == after_) return;
      message_.Clear();
      message_.set_sequence(snapshot->sequence);
      *message_.mutable_topology() = snapshot->topology;
      writing_sequence_ = snapshot->sequence;
      if (!write_finish_.try_start_write()) return;
      StartWrite(&message_);
    }
    void request_finish(::grpc::Status status) {
      subscription_.reset();
      pending_.reset();
      write_finish_.request_finish(std::move(status));
      finish_if_ready();
    }
    bool finish_if_ready() {
      auto status = write_finish_.take_finish_status();
      if (!status) return false;
      gate_->finish([status = std::move(*status)](TopologyReactor& reactor) {
        reactor.subscription_.reset();
        reactor.Finish(status);
      });
      return true;
    }
    HalServiceImpl& service_;
    std::uint64_t after_ = 0, writing_sequence_ = 0;
    bool admitted_ = false;
    WatchHalTopologyEvent message_;
    std::shared_ptr<const TopologySnapshot> pending_;
    SubscriptionHub<std::shared_ptr<const TopologySnapshot>>::Subscription
        subscription_;
    DeferredWriteFinish write_finish_;
    std::shared_ptr<LifetimeGate<TopologyReactor>> gate_;
    ActiveCallbackRegistry::Registration registration_;
  };

  void timer_loop() {
    auto next_topology = std::chrono::steady_clock::now();
    while (!stopping_.load()) {
      const auto now = std::chrono::steady_clock::now();
      const bool refresh_topology = now >= next_topology;
      if (refresh_topology) next_topology = now + topology_period_;
      worker_.submit([this, refresh_topology, now] {
        remote_components_.sample(now);
        sample_telemetry();
        if (!refresh_topology) return;
        try {
          const auto snapshot = refresh_topology_snapshot();
          if (snapshot->sequence > last_published_topology_) {
            last_published_topology_ = snapshot->sequence;
            topology_wakes_.publish(snapshot);
          }
        } catch (const HalAdapterError&) {
          last_published_topology_ = 0;
        }
      });
      remote_components_.tick_streams(now);
      std::unique_lock lock(timer_mutex_);
      timer_condition_.wait_for(lock, std::chrono::milliseconds(50),
                                [this] { return stopping_.load(); });
    }
  }

  std::shared_ptr<const TopologySnapshot> refresh_topology_snapshot() {
    linuxcnc::v1::HalTopology current;
    encode_hal_topology(adapter_.topology(), &current);
    auto structural = current;
    for (auto& pin : *structural.mutable_pins()) pin.clear_value();
    for (auto& parameter : *structural.mutable_params())
      parameter.clear_value();
    for (auto& signal : *structural.mutable_signals()) signal.clear_value();
    for (auto& function : *structural.mutable_functions()) {
      function.clear_runtime();
      function.clear_max_runtime();
      function.clear_max_runtime_increased();
    }
    for (auto& thread : *structural.mutable_threads()) {
      thread.clear_runtime();
      thread.clear_max_runtime();
    }
    const auto serialized = structural.SerializeAsString();
    std::lock_guard lock(topology_mutex_);
    if (!topology_snapshot_ || serialized != topology_serialized_) {
      ++topology_sequence_;
      topology_serialized_ = serialized;
    }
    auto snapshot = std::make_shared<TopologySnapshot>();
    snapshot->sequence = topology_sequence_;
    snapshot->topology = std::move(current);
    topology_snapshot_ = std::move(snapshot);
    return topology_snapshot_;
  }

  std::shared_ptr<const TopologySnapshot> cached_topology_snapshot() {
    std::lock_guard lock(topology_mutex_);
    return topology_snapshot_;
  }

  LinuxCncHalAdapter adapter_;
  BoundedExecutor& worker_;
  AdmissionCounter& stream_admission_;
  std::shared_ptr<HalValueTelemetry> telemetry_;
  const std::chrono::milliseconds topology_period_;
  ActiveCallbackRegistry callbacks_;
  RemoteComponentRpc remote_components_;
  SubscriptionHub<std::shared_ptr<const TopologySnapshot>> topology_wakes_;
  std::mutex topology_mutex_;
  std::string topology_serialized_;
  std::shared_ptr<const TopologySnapshot> topology_snapshot_;
  std::uint64_t topology_sequence_ = 0;
  std::uint64_t last_published_topology_ = 0;
  std::atomic<bool> writer_ready_{true};
  std::atomic<bool> stopping_{false};
  std::mutex timer_mutex_;
  std::condition_variable timer_condition_;
  std::thread timer_;
};

}  // namespace

std::unique_ptr<ManagedGrpcService> make_hal_service(
    const DaemonConfig& config, BoundedExecutor& worker,
    AdmissionCounter& component_admission, AdmissionCounter& stream_admission,
    std::shared_ptr<HalValueTelemetry> telemetry) {
  return std::make_unique<HalServiceImpl>(config, worker, component_admission,
                                          stream_admission,
                                          std::move(telemetry));
}

}  // namespace linuxcnc::server::detail

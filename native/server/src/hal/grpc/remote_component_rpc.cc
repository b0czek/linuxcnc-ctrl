#include "hal/grpc/remote_component_rpc.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "grpc/server/deferred_write_finish.hpp"
#include "hal/grpc/component_outbox.hpp"
#include "hal/grpc/remote_component_registry.hpp"

namespace linuxcnc::server::detail {
namespace {

using namespace linuxcnc::v1;

}  // namespace

struct RemoteComponentRpc::Impl {
  class Reactor final
      : public ::grpc::ServerBidiReactor<HalComponentClientMessage,
                                         HalComponentServerMessage> {
   public:
    explicit Reactor(Impl& owner)
        : owner_(owner),
          stream_admitted_(owner_.stream_admission.acquire()),
          first_message_deadline_(std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5)),
          gate_(std::make_shared<LifetimeGate<Reactor>>(this)) {
      const std::weak_ptr<LifetimeGate<Reactor>> weak = gate_;
      registration_ = owner_.callbacks.register_callback([weak] {
        if (auto gate = weak.lock())
          gate->invoke([](Reactor& reactor) { reactor.shutdown(); });
      });
      if (!registration_) {
        shutdown();
        return;
      }
      if (!stream_admitted_) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "stream admission limit reached"});
        return;
      }
      owner_.register_stream(gate_);
      StartRead(&request_);
    }

    void OnReadDone(bool ok) override {
      gate_->invoke([ok](Reactor& reactor) { reactor.read_done(ok); });
    }

    void OnWriteDone(bool ok) override {
      gate_->invoke([ok](Reactor& reactor) {
        reactor.write_finish_.complete_write(ok);
        if (reactor.finish_if_ready() || !ok) return;
        if (reactor.active_response_) reactor.resume_read_when_idle_ = true;
        if (!reactor.outbox_.empty()) {
          reactor.start_write(reactor.outbox_.pop_front());
        } else if (reactor.finish_after_responses_) {
          reactor.request_finish(::grpc::Status::OK);
        } else if (reactor.resume_read_when_idle_) {
          reactor.resume_read_when_idle_ = false;
          reactor.StartRead(&reactor.request_);
        }
      });
    }

    void OnCancel() override {
      gate_->invoke([](Reactor& reactor) {
        reactor.detach_transport();
        reactor.request_finish(
            {::grpc::StatusCode::CANCELLED, "component session cancelled"});
      });
    }

    void OnDone() override {
      gate_->detach();
      registration_.reset();
      detach_transport();
      if (stream_admitted_) owner_.stream_admission.release();
      delete this;
    }

    void shutdown() {
      detach_transport();
      request_finish({::grpc::StatusCode::UNAVAILABLE,
                      "server shutting down"});
    }

    void tick(std::chrono::steady_clock::time_point now) {
      if (!received_first_message_ && now >= first_message_deadline_) {
        request_finish(
            {::grpc::StatusCode::DEADLINE_EXCEEDED,
             "first component message was not received within 5 seconds"});
      }
    }

    void heartbeat_timeout(std::uint64_t generation) {
      if (generation_ == generation) {
        detach_requested_ = true;
        request_finish({::grpc::StatusCode::DEADLINE_EXCEEDED,
                        "component heartbeat timed out"});
      }
    }

    void offer_delta(HalComponentServerMessage message) {
      if (write_finish_.termination_requested()) return;
      if (write_finish_.write_in_flight()) {
        outbox_.push_delta(std::move(message));
        return;
      }
      start_write({std::move(message), false});
    }

   private:
    RemoteComponentCallbacks callbacks() const {
      const std::weak_ptr<LifetimeGate<Reactor>> weak = gate_;
      RemoteComponentCallbacks result;
      result.offer_delta = [weak](HalComponentServerMessage message) {
        if (auto gate = weak.lock())
          gate->invoke(
              [message = std::move(message)](Reactor& reactor) mutable {
                reactor.offer_delta(std::move(message));
              });
      };
      result.heartbeat_timeout = [weak](std::uint64_t generation) {
        if (auto gate = weak.lock())
          gate->invoke([generation](Reactor& reactor) {
            reactor.heartbeat_timeout(generation);
          });
      };
      return result;
    }

    void offer_response(HalComponentServerMessage message) {
      if (write_finish_.termination_requested()) return;
      if (write_finish_.write_in_flight()) {
        outbox_.push_response(std::move(message));
        return;
      }
      start_write({std::move(message), true});
    }

    void start_write(ComponentOutbox::Entry entry) {
      response_ = std::move(entry.message);
      active_response_ = entry.resume_read;
      if (!write_finish_.try_start_write()) return;
      StartWrite(&response_);
    }

    void read_done(bool ok) {
      if (write_finish_.termination_requested()) return;
      if (!ok) {
        detach_transport();
        request_finish(::grpc::Status::OK);
        return;
      }
      received_first_message_ = true;
      auto request = request_;
      request_.Clear();
      const std::weak_ptr<LifetimeGate<Reactor>> weak = gate_;
      const auto proxy_id = proxy_id_;
      const auto generation = generation_;
      auto component_callbacks = callbacks();
      if (!owner_.worker.submit(
              [owner = &owner_, weak, proxy_id, generation,
               callbacks = std::move(component_callbacks),
               request = std::move(request)]() mutable {
                auto result = owner->registry.consume(
                    proxy_id, generation, std::move(callbacks), request);
                const auto cleanup_id = result.proxy_id;
                const auto cleanup_generation = result.generation;
                bool delivered = false;
                if (auto gate = weak.lock())
                  delivered = gate->invoke(
                      [result = std::move(result)](Reactor& reactor) mutable {
                        if (!result.status.ok()) {
                          reactor.detach_transport();
                          reactor.request_finish(result.status);
                          return;
                        }
                        if (!result.proxy_id.empty()) {
                          reactor.proxy_id_ = std::move(result.proxy_id);
                          reactor.generation_ = result.generation;
                        }
                        if (result.response) {
                          reactor.offer_response(std::move(*result.response));
                          if (result.close) {
                            reactor.detach_requested_ = true;
                            reactor.finish_after_responses_ = true;
                          }
                        } else if (result.close) {
                          reactor.request_finish(::grpc::Status::OK);
                        } else if (!reactor.write_finish_
                                        .termination_requested()) {
                          reactor.StartRead(&reactor.request_);
                        }
                      });
                if (!delivered && !cleanup_id.empty())
                  owner->registry.detach(cleanup_id, cleanup_generation);
              })) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "HAL runtime queue is full"});
      }
    }

    void detach_transport() {
      if (detach_requested_ || proxy_id_.empty()) return;
      detach_requested_ = true;
      const auto proxy_id = proxy_id_;
      const auto generation = generation_;
      owner_.worker.submit_cleanup([owner = &owner_, proxy_id, generation] {
        owner->registry.detach(proxy_id, generation);
      });
    }

    void request_finish(::grpc::Status status) {
      outbox_ = {};
      resume_read_when_idle_ = false;
      write_finish_.request_finish(std::move(status));
      finish_if_ready();
    }

    bool finish_if_ready() {
      auto status = write_finish_.take_finish_status();
      if (!status) return false;
      gate_->finish([status = std::move(*status)](Reactor& reactor) {
        reactor.Finish(status);
      });
      return true;
    }

    Impl& owner_;
    bool stream_admitted_ = false;
    bool active_response_ = false;
    bool resume_read_when_idle_ = false;
    bool received_first_message_ = false;
    bool finish_after_responses_ = false;
    bool detach_requested_ = false;
    std::chrono::steady_clock::time_point first_message_deadline_;
    std::string proxy_id_;
    std::uint64_t generation_ = 0;
    HalComponentClientMessage request_;
    HalComponentServerMessage response_;
    ComponentOutbox outbox_;
    DeferredWriteFinish write_finish_;
    std::shared_ptr<LifetimeGate<Reactor>> gate_;
    ActiveCallbackRegistry::Registration registration_;
  };

  BoundedExecutor& worker;
  AdmissionCounter& stream_admission;
  ActiveCallbackRegistry& callbacks;
  RemoteComponentRegistry registry;
  std::mutex streams_mutex;
  std::vector<std::weak_ptr<LifetimeGate<Reactor>>> streams;

  Impl(LinuxCncHalAdapter& adapter, BoundedExecutor& worker,
       AdmissionCounter& component_admission,
       AdmissionCounter& stream_admission, ActiveCallbackRegistry& callbacks,
       const DaemonConfig& config)
      : worker(worker),
        stream_admission(stream_admission),
        callbacks(callbacks),
        registry(adapter, component_admission, config) {}

  void register_stream(const std::shared_ptr<LifetimeGate<Reactor>>& gate) {
    std::lock_guard lock(streams_mutex);
    streams.push_back(gate);
  }

  void tick_streams(std::chrono::steady_clock::time_point now) {
    std::vector<std::weak_ptr<LifetimeGate<Reactor>>> current;
    {
      std::lock_guard lock(streams_mutex);
      streams.erase(
          std::remove_if(streams.begin(), streams.end(),
                         [](const auto& stream) { return stream.expired(); }),
          streams.end());
      current = streams;
    }
    for (const auto& stream : current)
      if (auto gate = stream.lock())
        gate->invoke([now](Reactor& reactor) { reactor.tick(now); });
  }
};

RemoteComponentRpc::RemoteComponentRpc(
    LinuxCncHalAdapter& adapter, BoundedExecutor& worker,
    AdmissionCounter& component_admission, AdmissionCounter& stream_admission,
    ActiveCallbackRegistry& callbacks, const DaemonConfig& config)
    : impl_(std::make_unique<Impl>(adapter, worker, component_admission,
                                  stream_admission, callbacks, config)) {}

RemoteComponentRpc::~RemoteComponentRpc() = default;

::grpc::ServerBidiReactor<HalComponentClientMessage,
                          HalComponentServerMessage>*
RemoteComponentRpc::run() {
  return new Impl::Reactor(*impl_);
}

void RemoteComponentRpc::sample(std::chrono::steady_clock::time_point now) {
  impl_->registry.sample(now);
}

void RemoteComponentRpc::tick_streams(
    std::chrono::steady_clock::time_point now) {
  impl_->tick_streams(now);
}

void RemoteComponentRpc::shutdown_registry() { impl_->registry.shutdown(); }

}  // namespace linuxcnc::server::detail

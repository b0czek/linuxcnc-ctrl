#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "grpc/server/service_factories.hpp"
#include "grpc/server/unary_task_reactor.hpp"
#include "linuxcnc/v1/scope.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/scope/controller.hpp"
#include "linuxcnc_grpc/scope/telemetry.hpp"

namespace linuxcnc::server::detail {
namespace {

using namespace linuxcnc::v1;
constexpr char kScopeOwner[] = "grpc-control-plane";

ScopeRuntimeState encode_scope_state(ScopeState state) {
  switch (state) {
    case ScopeState::Idle:
      return SCOPE_RUNTIME_STATE_IDLE;
    case ScopeState::Init:
      return SCOPE_RUNTIME_STATE_INIT;
    case ScopeState::PreTrigger:
      return SCOPE_RUNTIME_STATE_PRE_TRIGGER;
    case ScopeState::TriggerWait:
      return SCOPE_RUNTIME_STATE_TRIGGER_WAIT;
    case ScopeState::PostTrigger:
      return SCOPE_RUNTIME_STATE_POST_TRIGGER;
    case ScopeState::Done:
      return SCOPE_RUNTIME_STATE_DONE;
    case ScopeState::Reset:
      return SCOPE_RUNTIME_STATE_RESET;
    case ScopeState::Invalid:
      return SCOPE_RUNTIME_STATE_INVALID;
  }
  return SCOPE_RUNTIME_STATE_INVALID;
}

void encode_scope_status(const ScopeStatus& source,
                         linuxcnc::v1::ScopeStatus* target) {
  target->set_state(encode_scope_state(source.state));
  target->set_buffer_length(
      static_cast<std::uint32_t>(std::max(0, source.buffer_length)));
  target->set_record_length(
      static_cast<std::uint32_t>(std::max(0, source.record_length)));
  target->set_sample_length(
      static_cast<std::uint32_t>(std::max(0, source.sample_length)));
  target->set_samples(static_cast<std::uint32_t>(std::max(0, source.samples)));
  target->set_start(static_cast<std::uint32_t>(std::max(0, source.start)));
  target->set_multiplier(
      static_cast<std::uint32_t>(std::max(0, source.multiplier)));
  target->set_watchdog(
      static_cast<std::uint32_t>(std::max(0, source.watchdog)));
  target->set_thread_name(source.thread_name);
  target->set_sample_period_ns(static_cast<std::uint64_t>(
      std::max<std::int64_t>(0, source.sample_period_ns)));
}

ScopeConfig decode_config(const ScopeAcquisitionConfig& wire) {
  ScopeConfig config;
  config.thread_name = wire.thread_name();
  config.multiplier = static_cast<int>(wire.multiplier());
  config.pre_trigger = static_cast<int>(wire.pre_trigger());
  config.trigger_channel = static_cast<int>(wire.trigger_channel());
  config.trigger_level = wire.trigger_level();
  config.rising = wire.rising();
  config.automatic = wire.automatic();
  std::array<bool, kScopeChannelCount> assigned{};
  for (const auto& channel : wire.channels()) {
    if (channel.index() >= kScopeChannelCount || assigned[channel.index()])
      throw ScopeControllerError("invalid or duplicate scope channel index",
                                 -EINVAL);
    assigned[channel.index()] = true;
    auto& target = config.channels[channel.index()];
    target.enabled = channel.enabled();
    if (!channel.enabled()) continue;
    if (channel.item().name().empty())
      throw ScopeControllerError("enabled scope channel requires a HAL item",
                                 -EINVAL);
    target.source.name = channel.item().name();
    switch (channel.item().kind()) {
      case HAL_ITEM_KIND_PIN:
        target.source.kind = ScopeSourceKind::Pin;
        break;
      case HAL_ITEM_KIND_PARAM:
        target.source.kind = ScopeSourceKind::Param;
        break;
      case HAL_ITEM_KIND_SIGNAL:
        target.source.kind = ScopeSourceKind::Signal;
        break;
      default:
        throw ScopeControllerError("unsupported scope HAL item kind", -EINVAL);
    }
  }
  return config;
}

::grpc::Status scope_error(const ScopeControllerError& error) {
  const auto code = error.code() == -EBUSY
                        ? ::grpc::StatusCode::RESOURCE_EXHAUSTED
                    : (error.code() == -EINVAL || error.code() == -ERANGE)
                        ? ::grpc::StatusCode::INVALID_ARGUMENT
                    : (error.code() == -ENOENT || error.code() == -ENODEV)
                        ? ::grpc::StatusCode::UNAVAILABLE
                        : ::grpc::StatusCode::FAILED_PRECONDITION;
  return {code, error.what()};
}

class ScopeServiceImpl final : public ScopeService::CallbackService,
                               public ManagedGrpcService {
 public:
  ScopeServiceImpl(const DaemonConfig& config, BoundedExecutor& worker,
                   std::shared_ptr<ScopeTelemetry> scope_telemetry)
      : worker_(worker),
        scope_telemetry_(std::move(scope_telemetry)),
        poll_period_(config.scope_period),
        heartbeat_period_(config.scope_heartbeat),
        requested_samples_(config.scope_samples) {}

  ~ScopeServiceImpl() override {
    if (!controller_) return;
    scope_telemetry_->revoke(kScopeOwner);
    controller_->release(kScopeOwner);
  }

  ::grpc::Service* service() noexcept override { return this; }
  void shutdown() override { callbacks_.shutdown(); }

  ::grpc::ServerUnaryReactor* GetStatus(::grpc::CallbackServerContext*,
                                        const google::protobuf::Empty*,
                                        ScopeControlState* response) override {
    return run(response, [](LinuxCncScopeController&) {});
  }

  ::grpc::ServerUnaryReactor* Configure(::grpc::CallbackServerContext*,
                                        const ScopeConfigure* request,
                                        ScopeControlState* response) override {
    const auto config = decode_config(request->config());
    return run(response, [config](LinuxCncScopeController& controller) {
      controller.configure(kScopeOwner, config);
    });
  }

  ::grpc::ServerUnaryReactor* Run(::grpc::CallbackServerContext*,
                                  const ScopeRun* request,
                                  ScopeControlState* response) override {
    ScopeRunMode mode;
    switch (request->mode()) {
      case SCOPE_RUN_MODE_RUN:
        mode = ScopeRunMode::Run;
        break;
      case SCOPE_RUN_MODE_SINGLE:
        mode = ScopeRunMode::Single;
        break;
      case SCOPE_RUN_MODE_ROLL:
        mode = ScopeRunMode::Roll;
        break;
      default:
        return run(response, [](LinuxCncScopeController&) {
          throw ScopeControllerError("scope run mode is required", -EINVAL);
        });
    }
    return run(response, [mode](LinuxCncScopeController& controller) {
      controller.run(kScopeOwner, mode);
    });
  }

  ::grpc::ServerUnaryReactor* Stop(::grpc::CallbackServerContext*,
                                   const ScopeStop*,
                                   ScopeControlState* response) override {
    return run(response, [](LinuxCncScopeController& controller) {
      controller.stop(kScopeOwner);
    });
  }

  ::grpc::ServerUnaryReactor* Trigger(::grpc::CallbackServerContext*,
                                      const ScopeTrigger*,
                                      ScopeControlState* response) override {
    return run(response, [](LinuxCncScopeController& controller) {
      controller.trigger(kScopeOwner);
    });
  }

 private:
  template <typename Operation>
  ::grpc::ServerUnaryReactor* run(ScopeControlState* response,
                                  Operation operation) {
    return new UnaryTaskReactor<ScopeControlState>(
        worker_, callbacks_, response,
        [this, operation = std::move(operation)](
            [[maybe_unused]] const std::stop_token& token,
            ScopeControlState* output) {
          try {
            auto& controller = ensure_controller();
            operation(controller);
            encode_scope_status(controller.status(), output->mutable_status());
            output->set_websocket_path(websocket_path_);
            return ::grpc::Status::OK;
          } catch (const ScopeControllerError& error) {
            return scope_error(error);
          }
        });
  }

  LinuxCncScopeController& ensure_controller() {
    if (controller_) return *controller_;
    auto controller = std::make_shared<LinuxCncScopeController>(
        "linuxcnc-grpc-scope", poll_period_, heartbeat_period_,
        requested_samples_);
    if (!controller->acquire(kScopeOwner))
      throw ScopeControllerError("scope controller is unavailable", -EBUSY);
    const auto path =
        scope_telemetry_->register_controller(kScopeOwner, controller);
    if (path.empty()) {
      controller->release(kScopeOwner);
      throw ScopeControllerError("scope telemetry is unavailable", -ENODEV);
    }
    websocket_path_ = path;
    controller_ = std::move(controller);
    return *controller_;
  }

  BoundedExecutor& worker_;
  std::shared_ptr<ScopeTelemetry> scope_telemetry_;
  std::shared_ptr<LinuxCncScopeController> controller_;
  std::string websocket_path_;
  const std::chrono::milliseconds poll_period_, heartbeat_period_;
  const std::size_t requested_samples_;
  ActiveCallbackRegistry callbacks_;
};

}  // namespace

std::unique_ptr<ManagedGrpcService> make_scope_service(
    const DaemonConfig& config, BoundedExecutor& worker,
    std::shared_ptr<ScopeTelemetry> scope_telemetry) {
  return std::make_unique<ScopeServiceImpl>(config, worker,
                                            std::move(scope_telemetry));
}

}  // namespace linuxcnc::server::detail

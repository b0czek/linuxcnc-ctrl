#include "grpc/server/runtime.hpp"

#include <grpcpp/grpcpp.h>
#include <pthread.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "grpc/server/local_health.hpp"
#include "grpc/server/service.hpp"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"
#include "linuxcnc_grpc/position/telemetry.hpp"
#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

namespace linuxcnc::server::detail {
namespace {

template <typename Function>
void invoke_shutdown(const char* name, Function&& function) noexcept {
  try {
    function();
  } catch (const std::exception& exception) {
    std::cerr << "linuxcnc-grpc-server: shutdown step " << name
              << " failed: " << exception.what() << '\n';
  } catch (...) {
    std::cerr << "linuxcnc-grpc-server: shutdown step " << name
              << " failed with an unknown exception\n";
  }
}

}  // namespace

ServerRuntime::ServerRuntime(
    std::unique_ptr<::grpc::Server> server,
    std::unique_ptr<ManagedGrpcService> machine,
    std::unique_ptr<ManagedGrpcService> ini,
    std::unique_ptr<ManagedGrpcService> program,
    std::unique_ptr<ManagedGrpcService> hal,
    std::unique_ptr<ManagedGrpcService> scope,
    std::unique_ptr<TelemetryWebSocketServer> telemetry_websocket,
    std::shared_ptr<PositionTelemetry> position_telemetry,
    std::shared_ptr<HalValueTelemetry> hal_telemetry,
    AdmissionCounter& stream_admission, AdmissionCounter& upload_admission,
    AdmissionCounter& component_admission, BoundedExecutor& blocking,
    BoundedExecutor& parser_worker, BoundedExecutor& hal_worker,
    BoundedExecutor& scope_worker)
    : server_(std::move(server)),
      machine_(std::move(machine)),
      ini_(std::move(ini)),
      program_(std::move(program)),
      hal_(std::move(hal)),
      scope_(std::move(scope)),
      telemetry_websocket_(std::move(telemetry_websocket)),
      position_telemetry_(std::move(position_telemetry)),
      hal_telemetry_(std::move(hal_telemetry)),
      stream_admission_(stream_admission),
      upload_admission_(upload_admission),
      component_admission_(component_admission),
      blocking_(blocking),
      parser_worker_(parser_worker),
      hal_worker_(hal_worker),
      scope_worker_(scope_worker) {}

ServerRuntime::~ServerRuntime() noexcept {
  request_shutdown();
  finalize();
}

void ServerRuntime::start_local_health(const std::string& endpoint) {
  if (endpoint.empty()) return;
  if (!server_ || local_health_ || shutdown_requested_.load() ||
      control_thread_.joinable())
    throw std::logic_error(
        "local health must start once after public startup and before the "
        "control thread");
  local_health_ = std::make_unique<LocalHealthServer>(endpoint);
}

void ServerRuntime::start_control_thread(const sigset_t& shutdown_signals) {
  if (control_thread_.joinable()) {
    throw std::logic_error("gRPC server control thread already started");
  }
  shutdown_signals_ = shutdown_signals;
  control_thread_ = std::thread([this] { run_control_thread(); });
}

void ServerRuntime::wait() {
  if (server_) server_->Wait();
}

void ServerRuntime::run_control_thread() noexcept {
  int signal = 0;
  if (sigwait(&shutdown_signals_, &signal) != 0) {
    std::cerr << "linuxcnc-grpc-server: failed waiting for shutdown signal\n";
  }
  request_shutdown();
}

void ServerRuntime::request_shutdown() noexcept {
  if (shutdown_requested_.exchange(true, std::memory_order_acq_rel)) return;

  // Keep this order aligned with the callback lifetime edge: stop new work,
  // stop auxiliary transport activity, close telemetry, then end callbacks,
  // and finally ask gRPC to leave Wait(). Every step is attempted even when a
  // user-provided/native implementation reports an exception.
  invoke_shutdown("local-health", [this] {
    if (local_health_) local_health_->shutdown();
  });
  invoke_shutdown("health-check", [this] {
    if (server_) {
      if (auto* health = server_->GetHealthCheckService()) health->Shutdown();
    }
  });
  invoke_shutdown("telemetry-websocket", [this] {
    if (telemetry_websocket_) telemetry_websocket_->stop();
  });
  invoke_shutdown("stream-admission", [this] { stream_admission_.stop(); });
  invoke_shutdown("upload-admission", [this] { upload_admission_.stop(); });
  invoke_shutdown("component-admission",
                  [this] { component_admission_.stop(); });
  invoke_shutdown("blocking-admission", [this] { blocking_.stop_admission(); });
  invoke_shutdown("parser-admission",
                  [this] { parser_worker_.stop_admission(); });
  invoke_shutdown("hal-admission", [this] { hal_worker_.stop_admission(); });
  invoke_shutdown("scope-worker-admission",
                  [this] { scope_worker_.stop_admission(); });
  invoke_shutdown("position-telemetry", [this] {
    if (position_telemetry_) position_telemetry_->close();
  });
  invoke_shutdown("hal-telemetry", [this] {
    if (hal_telemetry_) hal_telemetry_->close();
  });
  invoke_shutdown("machine-service", [this] {
    if (machine_) machine_->shutdown();
  });
  invoke_shutdown("ini-service", [this] {
    if (ini_) ini_->shutdown();
  });
  invoke_shutdown("program-service", [this] {
    if (program_) program_->shutdown();
  });
  invoke_shutdown("hal-service", [this] {
    if (hal_) hal_->shutdown();
  });
  invoke_shutdown("scope-service", [this] {
    if (scope_) scope_->shutdown();
  });
  invoke_shutdown("grpc-server", [this] {
    if (server_) {
      server_->Shutdown(std::chrono::system_clock::now() +
                        std::chrono::seconds(5));
    }
  });
  wake_control_thread();
}

void ServerRuntime::wake_control_thread() noexcept {
  if (!control_thread_.joinable() ||
      control_thread_.get_id() == std::this_thread::get_id()) {
    return;
  }
  // If the owner reaches finalize() because Wait() returned without the
  // signal waiter receiving SIGINT/SIGTERM, give sigwait() a wake-up event so
  // joining the dedicated control thread cannot hang indefinitely. Both signals
  // are blocked process-wide before this thread is started and are therefore
  // consumed by sigwait rather than invoking a process signal handler.
  // process-wide and consumed synchronously by sigwait() as a wake-up event.
  // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread): deliberate wake-up
  (void)pthread_kill(control_thread_.native_handle(), SIGTERM);
}

void ServerRuntime::join_control_thread() noexcept {
  if (!control_thread_.joinable()) return;
  if (control_thread_.get_id() == std::this_thread::get_id()) return;
  control_thread_.join();
}

void ServerRuntime::finalize() noexcept {
  request_shutdown();
  join_control_thread();
  if (finalized_.exchange(true, std::memory_order_acq_rel)) return;

  // The signal thread has completed the transport edge before this point, so
  // callbacks cannot enqueue new ordinary work while the queues are drained.
  invoke_shutdown("blocking-drain", [this] { blocking_.drain(); });
  invoke_shutdown("parser-drain", [this] { parser_worker_.drain(); });
  invoke_shutdown("hal-drain", [this] { hal_worker_.drain(); });
  invoke_shutdown("scope-drain", [this] { scope_worker_.drain(); });

  scope_.reset();
  hal_.reset();
  program_.reset();
  ini_.reset();
  machine_.reset();
  telemetry_websocket_.reset();
  position_telemetry_.reset();
  hal_telemetry_.reset();

  invoke_shutdown("parser-shutdown", [this] { parser_worker_.shutdown(); });
  invoke_shutdown("hal-shutdown", [this] { hal_worker_.shutdown(); });
  invoke_shutdown("scope-shutdown", [this] { scope_worker_.shutdown(); });
  invoke_shutdown("blocking-shutdown", [this] { blocking_.shutdown(); });

  server_.reset();
  local_health_.reset();
}

}  // namespace linuxcnc::server::detail

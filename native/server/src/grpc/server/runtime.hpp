#pragma once

#include <atomic>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

namespace grpc {
class Server;
}  // namespace grpc

namespace linuxcnc::server {

class AdmissionCounter;
class MachineHealthCache;
class BoundedExecutor;
class PositionTelemetry;
class HalValueTelemetry;
class TelemetryWebSocketServer;

namespace detail {

class ManagedGrpcService;
class LocalHealthServer;

// Owns the server-side shutdown edge. The signal thread and the main thread
// each participate in the normal one-shot lifecycle; an atomic gate ensures
// only the first request performs the ordered transport shutdown sequence.
// Repeated sequential finalization is harmless and drains workers only after
// the signal thread has joined.
class ServerRuntime {
 public:
  ServerRuntime(std::unique_ptr<::grpc::Server> server,
                std::unique_ptr<ManagedGrpcService> machine,
                std::unique_ptr<ManagedGrpcService> ini,
                std::unique_ptr<ManagedGrpcService> program,
                std::unique_ptr<ManagedGrpcService> hal,
                std::unique_ptr<ManagedGrpcService> scope,
                std::unique_ptr<TelemetryWebSocketServer> telemetry_websocket,
                std::shared_ptr<PositionTelemetry> position_telemetry,
                std::shared_ptr<HalValueTelemetry> hal_telemetry,
                AdmissionCounter& stream_admission,
                AdmissionCounter& upload_admission,
                AdmissionCounter& component_admission,
                BoundedExecutor& blocking, BoundedExecutor& parser_worker,
                BoundedExecutor& hal_worker, BoundedExecutor& scope_worker);

  ~ServerRuntime() noexcept;

  ServerRuntime(const ServerRuntime&) = delete;
  ServerRuntime& operator=(const ServerRuntime&) = delete;

  // Call on the owner thread after public startup, before the control thread.
  // Empty endpoints disable local health. The cache supplies machine state
  // for GET /health; null reports the machine as unavailable.
  void start_local_health(
      const std::string& endpoint,
      std::shared_ptr<const MachineHealthCache> machine = nullptr);

  // Starts the dedicated signal waiter. The caller must block the same
  // signal set in the thread that calls this method before starting it.
  void start_control_thread(const sigset_t& shutdown_signals);

  // Waits for gRPC's server state to terminate. If this returns without a
  // signal, the caller should still call finalize(), which passes through the
  // one-shot shutdown gate and joins the control thread before draining
  // workers.
  void wait();

  // Performs the ordered, no-throw transport shutdown edge. The signal thread
  // and runtime owner each call this in the normal lifecycle; the atomic gate
  // arbitrates that specific two-thread handoff. It is not a general
  // reentrant API.
  void request_shutdown() noexcept;

  // Joins the signal thread and drains/releases all runtime resources. Repeated
  // sequential calls are harmless; callers must not overlap finalization. The
  // destructor calls it as a final safety net.
  void finalize() noexcept;

 private:
  void run_control_thread() noexcept;
  void wake_control_thread() noexcept;
  void join_control_thread() noexcept;

  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<LocalHealthServer> local_health_;
  std::unique_ptr<ManagedGrpcService> machine_;
  std::unique_ptr<ManagedGrpcService> ini_;
  std::unique_ptr<ManagedGrpcService> program_;
  std::unique_ptr<ManagedGrpcService> hal_;
  std::unique_ptr<ManagedGrpcService> scope_;
  std::unique_ptr<TelemetryWebSocketServer> telemetry_websocket_;
  std::shared_ptr<PositionTelemetry> position_telemetry_;
  std::shared_ptr<HalValueTelemetry> hal_telemetry_;

  AdmissionCounter& stream_admission_;
  AdmissionCounter& upload_admission_;
  AdmissionCounter& component_admission_;
  BoundedExecutor& blocking_;
  BoundedExecutor& parser_worker_;
  BoundedExecutor& hal_worker_;
  BoundedExecutor& scope_worker_;

  sigset_t shutdown_signals_{};
  std::thread control_thread_;
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> finalized_{false};
};

}  // namespace detail
}  // namespace linuxcnc::server

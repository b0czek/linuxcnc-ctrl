#pragma once

#include <memory>

namespace linuxcnc::transport {
class ServerIdentity;
}

namespace linuxcnc::server {

struct DaemonConfig;
class PositionTelemetry;
class HalValueTelemetry;
class ScopeTelemetry;
class ProgramWorkspaceStore;
class BoundedExecutor;
class AdmissionCounter;

class TelemetryWebSocketServer {
 public:
  TelemetryWebSocketServer(
      const DaemonConfig& config,
      std::shared_ptr<PositionTelemetry> position_telemetry,
      std::shared_ptr<HalValueTelemetry> hal_telemetry,
      std::shared_ptr<ScopeTelemetry> scope_telemetry,
      std::shared_ptr<ProgramWorkspaceStore> workspaces,
      BoundedExecutor& parser_worker, AdmissionCounter& preview_admission,
      const std::shared_ptr<transport::ServerIdentity>& identity = nullptr);
  ~TelemetryWebSocketServer();

  TelemetryWebSocketServer(const TelemetryWebSocketServer&) = delete;
  TelemetryWebSocketServer& operator=(const TelemetryWebSocketServer&) = delete;

  void stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server

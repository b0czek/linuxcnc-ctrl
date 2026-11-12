#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <memory>

namespace linuxcnc::server {
struct DaemonConfig;
class PositionTelemetry;
class HalValueTelemetry;
class ScopeTelemetry;
class ProgramWorkspaceStore;
class BoundedExecutor;
class AdmissionCounter;

namespace detail {
class TelemetryStream;

// Listener lifecycle edge. All methods execute on the session's strand.
class TelemetrySession {
 public:
  virtual ~TelemetrySession() = default;
  virtual void run() = 0;
  virtual void stop() = 0;
  virtual boost::asio::any_io_executor get_executor() = 0;
};

std::shared_ptr<TelemetrySession> make_telemetry_session(
    TelemetryStream stream, const DaemonConfig& config,
    std::shared_ptr<PositionTelemetry> position,
    std::shared_ptr<HalValueTelemetry> hal,
    std::shared_ptr<ScopeTelemetry> scope,
    std::shared_ptr<ProgramWorkspaceStore> workspaces, BoundedExecutor& parser,
    AdmissionCounter& admission, std::function<void()> release);
}  // namespace detail
}  // namespace linuxcnc::server

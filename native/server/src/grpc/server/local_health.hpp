#pragma once

#include <memory>
#include <string>

namespace linuxcnc::server {
class MachineHealthCache;
}  // namespace linuxcnc::server

namespace linuxcnc::server::detail {

// Separate Unix-only HTTP/1.1 listener answering GET /health with server
// readiness and basic machine state as JSON. The caller starts it only after
// public listeners are ready. Requires a private parent directory and
// reclaims only unresponsive sockets. A null cache reports the machine as
// unavailable.
class LocalHealthServer {
 public:
  explicit LocalHealthServer(
      const std::string& endpoint,
      std::shared_ptr<const MachineHealthCache> machine = nullptr);
  ~LocalHealthServer();
  LocalHealthServer(const LocalHealthServer&) = delete;
  LocalHealthServer& operator=(const LocalHealthServer&) = delete;

  // Mark not serving and stop admission. Retain this object until
  // finalization; destruction removes its own socket.
  void shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace linuxcnc::server::detail

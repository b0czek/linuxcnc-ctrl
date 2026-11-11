#pragma once

#include <memory>
#include <string>

namespace linuxcnc::server::detail {

// Separate Unix-only listener. The caller starts it only after public listeners
// are ready. No application services or global builder plugins are registered.
// Requires a private parent directory and reclaims only unresponsive sockets.
class LocalHealthServer {
 public:
  explicit LocalHealthServer(const std::string& endpoint);
  ~LocalHealthServer();
  LocalHealthServer(const LocalHealthServer&) = delete;
  LocalHealthServer& operator=(const LocalHealthServer&) = delete;

  // Mark not serving and stop admission. Retain this object until finalization;
  // destruction waits for callbacks before removing its own socket.
  void shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace linuxcnc::server::detail

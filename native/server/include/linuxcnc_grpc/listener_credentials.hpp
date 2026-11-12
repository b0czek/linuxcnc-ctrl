#pragma once

#include <memory>

namespace grpc {
class ServerCredentials;
}
namespace linuxcnc::transport {
class ServerIdentity;
}  // namespace linuxcnc::transport

namespace linuxcnc::server {
struct DaemonConfig;

// Dedicated server-process startup operation. Call before SSL contexts, gRPC
// initialization, or worker threads. Identity and credentials are constructed
// before either listener binds. Both listeners must abandon TLS handshakes
// after transport::kServerHandshakeTimeout. Owners must stop all listeners
// before release.
struct ListenerCredentials {
  std::shared_ptr<transport::ServerIdentity> identity;
  std::shared_ptr<::grpc::ServerCredentials> grpc;
};

// Requires a configuration accepted by validate_config.
ListenerCredentials load_listener_credentials(const DaemonConfig& config);
}  // namespace linuxcnc::server

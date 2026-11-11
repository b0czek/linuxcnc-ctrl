#pragma once

#include <grpcpp/security/server_credentials.h>

#include <memory>

#include "linuxcnc_transport/credentials.hpp"

namespace grpc {
class ServerBuilder;
}

namespace linuxcnc::transport {

// Explicit daemon startup operation, before ANY SSL_CTX/gRPC initialization or
// threads. Installs in-memory process SSL defaults: TLS 1.3 and NumTickets=0.
// Does not write files, change environment, or configure another process.
// Dedicated server processes only: NumTickets is a server-only SSL_CONF command
// and later TLS_client_method() contexts can fail with config diagnostics
// enabled. Reserve process-wide OpenSSL SSL configuration for this module
// thereafter. The defaults remain installed until process exit. Repeated calls
// install nothing new and recheck the effective defaults.
void InitializeGrpcServerPolicy();

// Rechecks the process policy; the returned credentials retain the
// identity/provider.
std::shared_ptr<grpc::ServerCredentials> MakeGrpcServerCredentials(
    std::shared_ptr<ServerIdentity> identity);

// Makes the server abandon TLS handshakes after kServerHandshakeTimeout, as
// every listener serving an identity must. gRPC's own default is far longer.
void ApplyGrpcServerHandshakeTimeout(grpc::ServerBuilder& builder);

}  // namespace linuxcnc::transport

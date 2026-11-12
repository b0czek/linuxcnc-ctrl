#include "linuxcnc_grpc/listener_credentials.hpp"

#include <grpcpp/security/server_credentials.h>

#include <fstream>
#include <stdexcept>
#include <utility>

#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_transport/credentials.hpp"
#include "linuxcnc_transport/external_key.hpp"
#include "linuxcnc_transport/grpc_credentials.hpp"

namespace linuxcnc::server {
namespace {
std::string read_public_pem(const std::filesystem::path& path) {
  constexpr std::size_t max_bytes = std::size_t{1024} * 1024U;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read TLS input: " + path.string());
  std::string bytes(max_bytes + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(input.gcount()));
  if (bytes.empty() || bytes.size() > max_bytes || input.bad())
    throw std::runtime_error("invalid TLS input size: " + path.string());
  return bytes;
}
}  // namespace

ListenerCredentials load_listener_credentials(const DaemonConfig& config) {
  ListenerCredentials result;
  if (!config.tls) {
    result.grpc = ::grpc::InsecureServerCredentials();
    return result;
  }
  transport::InitializeGrpcServerPolicy();
  transport::ServerIdentityOptions options;
  options.certificate_chain_pem = read_public_pem(config.tls_certificate);
  if (!config.tls_client_ca.empty())
    options.client_roots_pem = read_public_pem(config.tls_client_ca);
  options.require_client_certificate = config.mtls;
  if (!config.tls_external_key_reference.empty()) {
    options.key = transport::DecodeExternalKey(
        read_public_pem(config.tls_external_key_reference));
  } else {
    options.key = transport::FilePrivateKey{config.tls_private_key.string()};
  }
  result.identity = transport::ServerIdentity::Load(
      std::move(options), config.tls_provider_module.string());
  result.grpc = transport::MakeGrpcServerCredentials(result.identity);
  return result;
}
}  // namespace linuxcnc::server

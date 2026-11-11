#include "linuxcnc_transport/grpc_credentials.hpp"

#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/server_builder.h>
#include <openssl/conf.h>

#include <mutex>

#include "bytes.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace linuxcnc::transport {
void InitializeGrpcServerPolicy() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    detail::Require(OPENSSL_init_ssl(0, nullptr) == 1,
                    "cannot initialize OpenSSL");
    // The policy is defined in memory, not read from the environment or a
    // file. It replaces any ssl_conf system default loaded from the process
    // OpenSSL configuration, and the check below confirms the result. The SSL
    // config module keeps its own copy after CONF_modules_load returns.
    constexpr char config[] =
        "openssl_conf = transport_init\n"
        "[transport_init]\nssl_conf = transport_ssl\n"
        "[transport_ssl]\nsystem_default = transport_server\n"
        "[transport_server]\nMinProtocol = TLSv1.3\nMaxProtocol = TLSv1.3\n"
        "NumTickets = 0\n";
    std::unique_ptr<CONF, decltype(&NCONF_free)> conf(NCONF_new(nullptr),
                                                      NCONF_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(config, sizeof(config) - 1), BIO_free);
    detail::Require(conf && bio &&
                        NCONF_load_bio(conf.get(), bio.get(), nullptr) == 1 &&
                        CONF_modules_load(conf.get(), nullptr, 0) == 1,
                    "cannot install gRPC server process TLS policy");
  });
  // Check the effective defaults before creating credentials. OpenSSL exposes
  // no callback for the SSL_CTX internally created by gRPC.
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> probe(
      SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  detail::Require(
      probe && SSL_CTX_get_min_proto_version(probe.get()) == TLS1_3_VERSION &&
          SSL_CTX_get_max_proto_version(probe.get()) == TLS1_3_VERSION &&
          SSL_CTX_get_num_tickets(probe.get()) == 0 &&
          SSL_CTX_get_max_early_data(probe.get()) == 0,
      "gRPC server process TLS defaults are not enforced");
}
std::shared_ptr<grpc::ServerCredentials> MakeGrpcServerCredentials(
    std::shared_ptr<ServerIdentity> identity) {
  detail::Require(identity != nullptr, "server identity is required");
  // Recheck policy in case the embedding process overwrote SSL defaults.
  InitializeGrpcServerPolicy();
#if LINUXCNC_TRANSPORT_GRPC_MODERN
  auto certificates =
      std::make_shared<grpc::experimental::InMemoryCertificateProvider>();
  if (!identity->client_roots_pem().empty())
    detail::Require(certificates->UpdateRoot(identity->client_roots_pem()).ok(),
                    "gRPC rejected client root certificates");
  std::vector<grpc::experimental::IdentityKeyOrSignerCertPair> pairs{
      {identity->key_pem(), identity->certificate_chain_pem()}};
  detail::Require(certificates->UpdateIdentityKeyCertPair(pairs).ok(),
                  "gRPC rejected server identity");
  auto result =
      grpc::experimental::TlsServerCredentialsOptions::Create(certificates);
  detail::Require(result.ok(), "gRPC rejected TLS credential options");
  auto options = *result;
  options.set_min_tls_version(TLS1_3);
  options.set_max_tls_version(TLS1_3);
  if (!identity->client_roots_pem().empty())
    options.set_root_certificate_provider(certificates);
#else
  std::vector<grpc::experimental::IdentityKeyCertPair> pairs{
      {identity->key_pem(), identity->certificate_chain_pem()}};
  auto certificates =
      std::make_shared<grpc::experimental::StaticDataCertificateProvider>(
          identity->client_roots_pem(), pairs);
  grpc::experimental::TlsServerCredentialsOptions options(certificates);
  options.watch_identity_key_cert_pairs();
  if (!identity->client_roots_pem().empty()) options.watch_root_certs();
#endif
  options.set_cert_request_type(
      identity->require_client_certificate()
          ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
          : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
  struct Owner {
    std::shared_ptr<ServerIdentity> identity;
    std::shared_ptr<grpc::ServerCredentials> credentials;
  };
  auto owner = std::make_shared<Owner>();
  owner->identity = std::move(identity);
  owner->credentials = grpc::experimental::TlsServerCredentials(options);
  detail::Require(owner->credentials != nullptr,
                  "cannot construct gRPC TLS credentials");
  return {owner, owner->credentials.get()};
}

void ApplyGrpcServerHandshakeTimeout(grpc::ServerBuilder& builder) {
  builder.AddChannelArgument("grpc.server_handshake_timeout_ms",
                             static_cast<int>(kServerHandshakeTimeout.count()));
}
}  // namespace linuxcnc::transport

#include "linuxcnc_transport/credentials.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/provider.h>

#include <fstream>
#include <vector>

#include "bytes.hpp"

namespace linuxcnc::transport {
namespace {
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
constexpr size_t kMaxPemSize = 1048576;
std::vector<Certificate> Certificates(std::string_view pem) {
  detail::Require(!pem.empty() && pem.size() <= kMaxPemSize,
                  "invalid certificate bundle size");
  std::vector<Certificate> certificates;
  while (!pem.empty()) {
    const auto first = pem.find_first_not_of(" \r\n\t");
    if (first == std::string_view::npos) break;
    pem.remove_prefix(first);
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    detail::Require(pem.substr(0, begin.size()) == begin,
                    "unexpected data in certificate bundle");
    const auto last = pem.find(end);
    detail::Require(last != std::string_view::npos, "truncated certificate");
    const auto block = pem.substr(0, last + end.size());
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(block.data(), static_cast<int>(block.size())),
        BIO_free);
    Certificate cert(
        bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr,
        X509_free);
    detail::Require(cert != nullptr, "invalid certificate PEM");
    certificates.push_back(std::move(cert));
    pem.remove_prefix(block.size());
  }
  detail::Require(!certificates.empty(), "empty certificate bundle");
  return certificates;
}
int NoPassword(char*, int, int, void*) { return 0; }
std::string ReadFileKey(const std::string& path) {
  detail::Require(!path.empty(), "empty private key file path");
  std::ifstream input(path, std::ios::binary);
  detail::Require(input.good(), "cannot open private key file");
  std::string pem(kMaxPemSize + 1, '\0');
  input.read(pem.data(), static_cast<std::streamsize>(pem.size()));
  pem.resize(static_cast<size_t>(input.gcount()));
  detail::Require(!pem.empty() && pem.size() <= kMaxPemSize,
                  "invalid private key file size");
  return pem;
}
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
Key ReadKey(const std::string& pem, bool external) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  Key key(bio ? PEM_read_bio_PrivateKey_ex(
                    bio.get(), nullptr, NoPassword, nullptr, nullptr,
                    external ? nullptr : "provider=default")
              : nullptr,
          EVP_PKEY_free);
  detail::Require(key != nullptr, "cannot decode selected server key");
  if (!external) {
    const auto* provider = EVP_PKEY_get0_provider(key.get());
    detail::Require(provider && std::string_view(OSSL_PROVIDER_get0_name(
                                    provider)) == "default",
                    "file key must be a private PEM key");
  }
  return key;
}
}  // namespace

struct ServerIdentity::Impl {
  using Provider =
      std::unique_ptr<OSSL_PROVIDER, decltype(&OSSL_PROVIDER_unload)>;
  // Destruction order: key first, then module.
  Provider external_provider{nullptr, OSSL_PROVIDER_unload};
  ServerIdentityOptions options;
  std::string pem;
  Key key{nullptr, EVP_PKEY_free};
};
ServerIdentity::ServerIdentity(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ServerIdentity::~ServerIdentity() = default;

std::shared_ptr<ServerIdentity> ServerIdentity::Load(
    ServerIdentityOptions options, const std::string& provider_module) {
  auto impl = std::make_shared<Impl>();
  const bool external =
      std::holds_alternative<ExternalKeyReference>(options.key);
  if (external) {
    detail::Require(!provider_module.empty() &&
                        provider_module.front() == '/' &&
                        provider_module.find('\0') == std::string::npos,
                    "external key requires an absolute provider module path");
    impl->pem = EncodeExternalKey(std::get<ExternalKeyReference>(options.key));
    // Keep OpenSSL's automatic default provider: loading a provider without
    // retaining fallbacks would leave the process without one once the
    // identity is released.
    impl->external_provider.reset(
        OSSL_PROVIDER_try_load(nullptr, provider_module.c_str(), 1));
    detail::Require(impl->external_provider != nullptr,
                    "cannot load external key provider");
  } else {
    impl->pem = ReadFileKey(std::get<FilePrivateKey>(options.key).path);
  }
  impl->key = ReadKey(impl->pem, external);
  auto chain = Certificates(options.certificate_chain_pem);
  detail::Require(
      X509_check_private_key(chain.front().get(), impl->key.get()) == 1,
      "certificate does not match server key");
  // Roots without verification would be loaded but never requested.
  detail::Require(
      options.require_client_certificate || options.client_roots_pem.empty(),
      "client trust roots require client certificate verification");
  if (options.require_client_certificate)
    (void)Certificates(options.client_roots_pem);
  impl->options = std::move(options);
  return std::shared_ptr<ServerIdentity>(new ServerIdentity(std::move(impl)));
}

void ServerIdentity::Configure(SSL_CTX* context) {
  detail::Require(context != nullptr, "null SSL_CTX");
  static const int index = SSL_CTX_get_ex_new_index(
      0, nullptr, nullptr, nullptr,
      [](void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
        delete static_cast<std::shared_ptr<Impl>*>(ptr);
      });
  detail::Require(index >= 0 && !SSL_CTX_get_ex_data(context, index),
                  "SSL_CTX identity already configured");
  detail::Require(
      SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) == 1 &&
          SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) == 1 &&
          SSL_CTX_set_num_tickets(context, 0) == 1 &&
          SSL_CTX_set_max_early_data(context, 0) == 1,
      "cannot enforce TLS server policy");
  SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
  auto chain = Certificates(impl_->options.certificate_chain_pem);
  detail::Require(SSL_CTX_use_certificate(context, chain.front().get()) == 1 &&
                      SSL_CTX_clear_chain_certs(context) == 1,
                  "cannot install server certificate");
  for (size_t i = 1; i < chain.size(); ++i)
    detail::Require(SSL_CTX_add1_chain_cert(context, chain[i].get()) == 1,
                    "cannot install server certificate chain");
  detail::Require(SSL_CTX_use_PrivateKey(context, impl_->key.get()) == 1 &&
                      SSL_CTX_check_private_key(context) == 1,
                  "cannot install server private key");
  // Replace the trust store instead of inheriting any prior trust decisions.
  std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(
      X509_STORE_new(), X509_STORE_free);
  detail::Require(store != nullptr, "cannot allocate client trust store");
  if (!impl_->options.client_roots_pem.empty()) {
    for (auto& root : Certificates(impl_->options.client_roots_pem))
      detail::Require(X509_STORE_add_cert(store.get(), root.get()) == 1,
                      "cannot install client trust root");
  }
  SSL_CTX_set_cert_store(context, store.release());
  SSL_CTX_set_verify(context,
                     impl_->options.require_client_certificate
                         ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                         : SSL_VERIFY_NONE,
                     nullptr);
  auto owner = std::make_unique<std::shared_ptr<Impl>>(impl_);
  detail::Require(SSL_CTX_set_ex_data(context, index, owner.get()) == 1,
                  "cannot retain server identity");
  [[maybe_unused]] auto* retained = owner.release();
}
const std::string& ServerIdentity::certificate_chain_pem() const {
  return impl_->options.certificate_chain_pem;
}
const std::string& ServerIdentity::client_roots_pem() const {
  return impl_->options.client_roots_pem;
}
const std::string& ServerIdentity::key_pem() const { return impl_->pem; }
bool ServerIdentity::require_client_certificate() const {
  return impl_->options.require_client_certificate;
}
}  // namespace linuxcnc::transport

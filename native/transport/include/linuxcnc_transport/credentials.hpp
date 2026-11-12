#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <variant>

#include "linuxcnc_transport/external_key.hpp"

namespace linuxcnc::transport {

// File keys are a separate explicit choice; external references never fall
// back.
struct FilePrivateKey {
  std::string path;
};
using ServerKey = std::variant<FilePrivateKey, ExternalKeyReference>;

struct ServerIdentityOptions {
  std::string certificate_chain_pem;
  std::string client_roots_pem;  // Must be empty without client verification.
  ServerKey key;
  bool require_client_certificate = true;
};

class ServerIdentity {
 public:
  // provider_module must be an absolute path for external keys, unused for
  // files.
  static std::shared_ptr<ServerIdentity> Load(
      ServerIdentityOptions options, const std::string& provider_module);
  ~ServerIdentity();
  ServerIdentity(const ServerIdentity&) = delete;
  ServerIdentity& operator=(const ServerIdentity&) = delete;

  // Fresh server context only. TLS 1.3, no tickets/cache/early data, matching
  // certificate/key, optional verified client certificates. Owns identity via
  // SSL_CTX ex-data until that context is destroyed. Throws on failure.
  void Configure(SSL_CTX* context);

  const std::string& certificate_chain_pem() const;
  const std::string& client_roots_pem() const;
  const std::string& key_pem() const;
  bool require_client_certificate() const;

 private:
  struct Impl;
  explicit ServerIdentity(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace linuxcnc::transport

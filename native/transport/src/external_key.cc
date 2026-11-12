#include "linuxcnc_transport/external_key.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/un.h>

#include <algorithm>
#include <memory>

#include "bytes.hpp"

namespace linuxcnc::transport {
namespace {
constexpr char kLabel[] = "LINUXCNC EXTERNAL TLS KEY";
void Validate(const ExternalKeyReference& key) {
  using detail::Require;
  Require(!key.socket_path.empty() && key.socket_path.front() == '/' &&
              key.socket_path.size() < sizeof(sockaddr_un::sun_path) &&
              key.socket_path.find('\0') == std::string::npos,
          "external key requires an absolute bounded Unix socket path");
  Require(detail::Identifier(key.key_id), "invalid external key identifier");
  Require(key.public_point.front() == 4, "expected uncompressed P-384 point");
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new_from_name(nullptr, "EC", "provider=default"),
      EVP_PKEY_CTX_free);
  auto point = key.public_point;
  char group[] = "secp384r1";
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
      OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                        point.size()),
      OSSL_PARAM_construct_end()};
  EVP_PKEY* raw = nullptr;
  Require(
      ctx && EVP_PKEY_fromdata_init(ctx.get()) > 0 &&
          EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params) > 0,
      "invalid external public key");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(raw,
                                                                 EVP_PKEY_free);
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> check(
      EVP_PKEY_CTX_new_from_pkey(nullptr, raw, "provider=default"),
      EVP_PKEY_CTX_free);
  Require(check && EVP_PKEY_public_check(check.get()) > 0,
          "external public point is not on P-384");
}
}  // namespace

std::string EncodeExternalKey(const ExternalKeyReference& key) {
  Validate(key);
  detail::Bytes bytes{'L', 'C', 'K', 2};
  bytes.push_back(static_cast<unsigned char>(key.socket_path.size()));
  bytes.push_back(static_cast<unsigned char>(key.key_id.size()));
  bytes.insert(bytes.end(), key.public_point.begin(), key.public_point.end());
  bytes.insert(bytes.end(), key.socket_path.begin(), key.socket_path.end());
  bytes.insert(bytes.end(), key.key_id.begin(), key.key_id.end());
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  detail::Require(bio && PEM_write_bio(bio.get(), kLabel, "", bytes.data(),
                                       static_cast<long>(bytes.size())) > 0,
                  "cannot encode external key");
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio.get(), &data);
  return {data, static_cast<size_t>(size)};
}

ExternalKeyReference DecodeExternalKey(std::string_view pem) {
  using detail::Require;
  Require(!pem.empty() && pem.size() <= 1024, "invalid external PEM size");
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  char* name = nullptr;
  char* header = nullptr;
  unsigned char* data = nullptr;
  long size = 0;
  const int ok =
      bio ? PEM_read_bio(bio.get(), &name, &header, &data, &size) : 0;
  const auto free_buffer = [](auto* p) { OPENSSL_free(p); };
  std::unique_ptr<char, decltype(free_buffer)> name_owner(name, free_buffer);
  std::unique_ptr<char, decltype(free_buffer)> header_owner(header,
                                                            free_buffer);
  std::unique_ptr<unsigned char, decltype(free_buffer)> data_owner(data,
                                                                   free_buffer);
  Require(ok > 0 && name && std::string_view(name) == kLabel && header &&
              *header == '\0' && size > 0,
          "invalid external key envelope");
  detail::Reader reader(
      {reinterpret_cast<const char*>(data), static_cast<size_t>(size)});
  Require(reader.Take(4) == std::string_view("LCK\2", 4),
          "unsupported external key version");
  ExternalKeyReference key;
  const auto path_size = reader.Byte();
  const auto id_size = reader.Byte();
  const auto point = reader.Take(97);
  std::copy(point.begin(), point.end(), key.public_point.begin());
  key.socket_path = reader.Take(path_size);
  key.key_id = reader.Take(id_size);
  reader.End();
  Require(EncodeExternalKey(key) == pem, "noncanonical external key envelope");
  return key;
}
}  // namespace linuxcnc::transport

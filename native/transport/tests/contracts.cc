#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "linuxcnc_transport/external_key.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace {
using namespace linuxcnc::transport;
using Bytes = std::vector<unsigned char>;
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void Reject(const std::function<void()>& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  Check(rejected, "invalid input accepted");
}
std::string_view View(const Bytes& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void Run() {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new_from_name(nullptr, "EC", "provider=default"),
      EVP_PKEY_CTX_free);
  Check(context && EVP_PKEY_keygen_init(context.get()) > 0 &&
            EVP_PKEY_CTX_set_group_name(context.get(), "secp384r1") > 0,
        "ephemeral keygen init");
  EVP_PKEY* raw = nullptr;
  Check(EVP_PKEY_generate(context.get(), &raw) > 0, "ephemeral keygen");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
  ExternalKeyReference reference;
  reference.key_id = "test.identity";
  reference.socket_path = "/tmp/unused-transport-contract.sock";
  size_t point_size = reference.public_point.size();
  Check(EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY,
                                        reference.public_point.data(),
                                        point_size, &point_size) > 0 &&
            point_size == 97,
        "public point export");
  auto pem = EncodeExternalKey(reference);
  std::unique_ptr<BIO, decltype(&BIO_free)> encoded(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  EVP_PKEY* undecodable =
      PEM_read_bio_PrivateKey(encoded.get(), nullptr, nullptr, nullptr);
  const bool accepted_without_provider = undecodable != nullptr;
  EVP_PKEY_free(undecodable);
  Check(!accepted_without_provider,
        "external reference accepted without provider");
  auto decoded = DecodeExternalKey(pem);
  Check(decoded.public_point == reference.public_point &&
            decoded.key_id == reference.key_id &&
            decoded.socket_path == reference.socket_path,
        "reference roundtrip");
  Reject([&] { DecodeExternalKey(pem + "trailing"); });
  Reject([&] { DecodeExternalKey("pkcs11:object=test"); });
  Reject([&] { DecodeExternalKey(pem.substr(0, pem.size() - 4)); });
  auto bad = reference;
  bad.public_point.fill(0);
  Reject([&] { EncodeExternalKey(bad); });
  bad = reference;
  bad.socket_path = "relative.sock";
  Reject([&] { EncodeExternalKey(bad); });

  TlsServerSignRequest request{"test.identity", TranscriptHash::kSha384,
                               Bytes(48, 0xa5)};
  const auto frame = EncodeSignRequest(request);
  const auto parsed = DecodeSignRequest(View(frame));
  Check(parsed.key_id == request.key_id && parsed.hash == request.hash &&
            parsed.transcript_hash == request.transcript_hash,
        "typed request roundtrip");
  // Independent RFC 8446 example, not an expected value computed by the codec.
  std::string expected(64, ' ');
  expected += "TLS 1.3, server CertificateVerify";
  expected.push_back('\0');
  expected.append(48, static_cast<char>(0xa5));
  Check(View(BuildServerCertificateVerify(parsed)) == expected,
        "server CertificateVerify encoding");
  for (size_t size = 0; size < frame.size(); ++size)
    Reject([&] { DecodeSignRequest(View(frame).substr(0, size)); });
  auto changed = frame;
  changed.push_back(0);
  Reject([&] { DecodeSignRequest(View(changed)); });
  for (size_t field : {size_t{5}, size_t{6}, size_t{7}, size_t{8}, size_t{9}}) {
    changed = frame;
    changed[field] = 0xff;
    Reject([&] { DecodeSignRequest(View(changed)); });
  }
  request.hash = TranscriptHash::kSha256;
  Reject([&] { EncodeSignRequest(request); });
  request.transcript_hash.resize(32);
  Check(BuildServerCertificateVerify(request).size() == 130,
        "SHA256 transcript size");
  Reject([&] { DecodeSignResponse(View(EncodeSignResponse({}))); });
  Reject([&] { EncodeSignResponse(Bytes(104, 0)); });
  // DER pair r=1,s=1 is structurally valid; r=0 is forbidden.
  const Bytes small_signature{0x30, 6, 2, 1, 1, 2, 1, 1};
  Check(DecodeSignResponse(View(EncodeSignResponse(small_signature))) ==
            small_signature,
        "canonical DER signature roundtrip");
  auto zero_signature = small_signature;
  zero_signature[4] = 0;
  Reject([&] { EncodeSignResponse(zero_signature); });
  request.key_id = "other";
  Reject([&] { RequestServerSignature(reference, request); });
  std::cout
      << "reference, typed purpose, framing, bounds and DER checks passed\n";
}
}  // namespace
int main() {
  try {
    Run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

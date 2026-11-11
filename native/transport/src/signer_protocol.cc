#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

#include <memory>

#include "bytes.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace linuxcnc::transport {
namespace {
void Validate(const TlsServerSignRequest& request) {
  detail::Require(detail::Identifier(request.key_id), "invalid signer key id");
  const auto size = request.transcript_hash.size();
  detail::Require((request.hash == TranscriptHash::kSha256 && size == 32) ||
                      (request.hash == TranscriptHash::kSha384 && size == 48),
                  "unsupported transcript hash");
}
detail::Bytes Frame(detail::Bytes payload) {
  detail::Require(payload.size() + 2 <= kMaxSignerRecord,
                  "oversize signer record");
  detail::Bytes result;
  detail::U16(result, static_cast<unsigned int>(payload.size()));
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}
detail::Reader Payload(std::string_view record) {
  detail::Require(record.size() <= kMaxSignerRecord, "oversize signer record");
  detail::Reader frame(record);
  const auto size = frame.U16();
  auto payload = frame.Take(size);
  frame.End();
  return detail::Reader(payload);
}
void ValidateSignature(const detail::Bytes& bytes) {
  detail::Require(bytes.size() >= 8 && bytes.size() <= 104,
                  "invalid ECDSA signature size");
  const auto* pos = bytes.data();
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(
      d2i_ECDSA_SIG(nullptr, &pos, static_cast<long>(bytes.size())),
      ECDSA_SIG_free);
  detail::Require(sig && pos == bytes.data() + bytes.size(),
                  "invalid ECDSA DER");
  std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group(
      EC_GROUP_new_by_curve_name(NID_secp384r1), EC_GROUP_free);
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig.get(), &r, &s);
  const BIGNUM* order = group ? EC_GROUP_get0_order(group.get()) : nullptr;
  detail::Require(order && !BN_is_negative(r) && !BN_is_zero(r) &&
                      !BN_is_negative(s) && !BN_is_zero(s) &&
                      BN_cmp(r, order) < 0 && BN_cmp(s, order) < 0,
                  "ECDSA scalar out of range");
  detail::Bytes canonical(bytes.size());
  auto* out = canonical.data();
  detail::Require(
      i2d_ECDSA_SIG(sig.get(), &out) == static_cast<int>(bytes.size()) &&
          canonical == bytes,
      "noncanonical ECDSA DER");
}
}  // namespace

std::vector<unsigned char> EncodeSignRequest(
    const TlsServerSignRequest& request) {
  Validate(request);
  detail::Bytes payload{'L',
                        'C',
                        'S',
                        1,
                        1,
                        5,
                        3,
                        static_cast<unsigned char>(request.hash),
                        static_cast<unsigned char>(request.key_id.size())};
  payload.insert(payload.end(), request.key_id.begin(), request.key_id.end());
  payload.insert(payload.end(), request.transcript_hash.begin(),
                 request.transcript_hash.end());
  return Frame(std::move(payload));
}

TlsServerSignRequest DecodeSignRequest(std::string_view record) {
  auto reader = Payload(record);
  detail::Require(reader.Take(7) == std::string_view("LCS\1\1\5\3", 7),
                  "unsupported signer version, operation, or scheme");
  TlsServerSignRequest request;
  request.hash = static_cast<TranscriptHash>(reader.Byte());
  const auto id_size = reader.Byte();
  request.key_id = reader.Take(id_size);
  const auto size = request.hash == TranscriptHash::kSha256 ? 32 : 48;
  auto digest = reader.Take(size);
  request.transcript_hash.assign(digest.begin(), digest.end());
  reader.End();
  Validate(request);
  return request;
}

std::vector<unsigned char> EncodeSignResponse(const detail::Bytes& signature) {
  if (!signature.empty()) ValidateSignature(signature);
  detail::Bytes payload{'L', 'C', 'S', 1,
                        static_cast<unsigned char>(signature.empty() ? 1 : 0)};
  payload.insert(payload.end(), signature.begin(), signature.end());
  return Frame(std::move(payload));
}

std::vector<unsigned char> DecodeSignResponse(std::string_view record) {
  auto reader = Payload(record);
  detail::Require(reader.Take(4) == std::string_view("LCS\1", 4),
                  "unsupported signer response version");
  detail::Require(reader.Byte() == 0, "external signer denied operation");
  auto signature = reader.Take(record.size() - 7);
  reader.End();
  detail::Bytes result(signature.begin(), signature.end());
  ValidateSignature(result);
  return result;
}

std::vector<unsigned char> BuildServerCertificateVerify(
    const TlsServerSignRequest& request) {
  Validate(request);
  detail::Bytes message;
  message.reserve(146);
  message.resize(64, 0x20);
  constexpr std::string_view context = "TLS 1.3, server CertificateVerify";
  message.insert(message.end(), context.begin(), context.end());
  message.push_back(0);
  message.insert(message.end(), request.transcript_hash.begin(),
                 request.transcript_hash.end());
  return message;
}
}  // namespace linuxcnc::transport

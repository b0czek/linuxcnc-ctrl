#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "linuxcnc_transport/external_key.hpp"

namespace linuxcnc::transport {

enum class TranscriptHash : uint8_t { kSha256 = 1, kSha384 = 2 };

// The sole supported operation is TLS 1.3 SERVER CertificateVerify, with
// ecdsa_secp384r1_sha384 (0x0503). No arbitrary-message/digest signing
// operation.
struct TlsServerSignRequest {
  std::string key_id;
  TranscriptHash hash;
  std::vector<unsigned char> transcript_hash;
};

// Wire records include a two-byte big-endian payload length. One request and
// response per connection. Decode functions require exactly one complete
// record.
inline constexpr size_t kMaxSignerRecord = 256;
std::vector<unsigned char> EncodeSignRequest(
    const TlsServerSignRequest& request);
TlsServerSignRequest DecodeSignRequest(std::string_view record);
std::vector<unsigned char> EncodeSignResponse(
    const std::vector<unsigned char>& der_signature);  // empty = denied
std::vector<unsigned char> DecodeSignResponse(std::string_view record);

// Backend reconstructs this message, then signs it with P-384/SHA-384. Backends
// must validate/authorize the typed request rather than accepting a raw digest.
std::vector<unsigned char> BuildServerCertificateVerify(
    const TlsServerSignRequest& request);

// Listeners serving these identities must abandon TLS handshakes after this
// deadline. The signer deadline is shorter, so a signature can still complete
// a handshake. It covers time queued at a serial signer, so a burst of
// concurrent handshakes can share one slow signer.
inline constexpr std::chrono::milliseconds kServerHandshakeTimeout{5000};
inline constexpr std::chrono::milliseconds kSignerTimeout{4000};
static_assert(kSignerTimeout < kServerHandshakeTimeout);

// AF_UNIX client; one kSignerTimeout deadline covers connect/write/read.
// Replies must contain canonical DER ECDSA with nonzero P-384-range r and s.
// Requests are not limited here. While the signer's listen backlog is full,
// connect waits for room within the deadline (Linux AF_UNIX semantics), so a
// serial signer with a small backlog still queues a burst of requests. `key`
// must already be valid, as returned by DecodeExternalKey or accepted by
// EncodeExternalKey; its public point is not rechecked here.
std::vector<unsigned char> RequestServerSignature(
    const ExternalKeyReference& key, const TlsServerSignRequest& request);

}  // namespace linuxcnc::transport

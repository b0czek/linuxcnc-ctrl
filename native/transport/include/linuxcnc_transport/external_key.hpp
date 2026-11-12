#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace linuxcnc::transport {

// Public data only. The endpoint must be protected by the caller's OS policy.
struct ExternalKeyReference {
  std::string socket_path;
  std::string key_id;
  std::array<unsigned char, 97> public_point{};  // SEC1 uncompressed P-384
};

// Strict, versioned PEM envelope, not a key URI or a private scalar.
// These and the other transport functions throw std::runtime_error on failure.
std::string EncodeExternalKey(const ExternalKeyReference& key);
ExternalKeyReference DecodeExternalKey(std::string_view pem);

}  // namespace linuxcnc::transport

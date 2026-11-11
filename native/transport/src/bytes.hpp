#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace linuxcnc::transport::detail {
using Bytes = std::vector<unsigned char>;
inline std::string_view View(const Bytes& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
inline void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
inline void U16(Bytes& out, unsigned int n) {
  out.push_back(static_cast<unsigned char>(n >> 8));
  out.push_back(static_cast<unsigned char>(n));
}
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  unsigned int Byte() {
    Require(!bytes_.empty(), "truncated record");
    const auto result = static_cast<unsigned char>(bytes_.front());
    bytes_.remove_prefix(1);
    return result;
  }
  unsigned int U16() {
    const auto high = Byte();
    return (high << 8) | Byte();
  }
  std::string_view Take(size_t n) {
    Require(n <= bytes_.size(), "truncated record");
    auto result = bytes_.substr(0, n);
    bytes_.remove_prefix(n);
    return result;
  }
  void End() const { Require(bytes_.empty(), "trailing record data"); }

 private:
  std::string_view bytes_;
};
inline bool Identifier(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return false;
  }
  return true;
}
}  // namespace linuxcnc::transport::detail

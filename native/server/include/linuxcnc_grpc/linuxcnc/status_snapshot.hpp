#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace linuxcnc::server {

struct PassiveStatusSnapshot {
  std::uint64_t task_heartbeat = 0;
  std::int32_t task_state = 0;
  std::int32_t interpreter_state = 0;
  bool in_position = false;
  std::string ini_filename;
};

// Only an explicitly passive libnml API may implement this reader. Unsupported
// builds report unavailable; there is no legacy NML or Python stat fallback.
// No tool data or process environment is consulted. Each sample is decoded by
// the matched CMS/EMC_STAT owner under this reader's balanced SEM_UNDO hold.
class PassiveStatusReader {
 public:
  explicit PassiveStatusReader(const std::filesystem::path& configuration);
  ~PassiveStatusReader();
  PassiveStatusReader(const PassiveStatusReader&) = delete;
  PassiveStatusReader& operator=(const PassiveStatusReader&) = delete;
  PassiveStatusSnapshot read();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server

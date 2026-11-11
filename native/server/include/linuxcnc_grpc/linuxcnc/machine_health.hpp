#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace linuxcnc::server {

struct NmlStatusSnapshot;

// The basic machine state reported by the local health endpoint.
struct MachineHealth {
  // False until the first status arrives and whenever the NML status channel
  // is stale or disconnected.
  bool available = false;
  // Time since the last new EMC_STAT. Task publishes every cycle, so a
  // growing age means task has stopped even if the channel is still open.
  std::chrono::milliseconds age{0};
  std::int32_t task_state = 0;
  std::int32_t interp_state = 0;
  bool in_position = false;
  std::string ini_filename;
};

// Latest machine state, written by the status poller and read by the local
// health endpoint. Thread-safe.
class MachineHealthCache {
 public:
  using Clock = std::chrono::steady_clock;

  void observe(const NmlStatusSnapshot& snapshot,
               Clock::time_point now = Clock::now());
  void mark_unavailable();
  MachineHealth read(Clock::time_point now = Clock::now()) const;

 private:
  mutable std::mutex mutex_;
  MachineHealth latest_;
  Clock::time_point observed_at_{};
};

// One-line JSON body for GET /health.
std::string format_health_json(bool serving, const MachineHealth& machine);

}  // namespace linuxcnc::server

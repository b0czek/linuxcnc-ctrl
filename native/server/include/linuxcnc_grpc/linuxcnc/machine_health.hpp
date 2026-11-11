#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace linuxcnc::server {

struct NmlStatusSnapshot;

// The basic machine state reported by the local health endpoint.
struct MachineHealth {
  // False until the first status arrives, whenever the NML status channel is
  // stale or disconnected, and once the last status is older than the cache's
  // stale_after limit.
  bool available = false;
  // Time since the poller last saw a new EMC_STAT. Task publishes every
  // cycle, so this stays below the poll period while task runs.
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

  // Reads report the machine unavailable once its last status is older than
  // stale_after, so a stopped task is not reported as running for long.
  explicit MachineHealthCache(
      std::chrono::milliseconds stale_after = std::chrono::milliseconds(500))
      : stale_after_(stale_after) {}

  void observe(const NmlStatusSnapshot& snapshot,
               Clock::time_point now = Clock::now());
  void mark_unavailable();
  MachineHealth read(Clock::time_point now = Clock::now()) const;

 private:
  const std::chrono::milliseconds stale_after_;
  mutable std::mutex mutex_;
  MachineHealth latest_;
  Clock::time_point observed_at_{};
};

// One-line JSON body for GET /health.
std::string format_health_json(bool serving, const MachineHealth& machine);

}  // namespace linuxcnc::server

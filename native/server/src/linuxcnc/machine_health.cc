#include "linuxcnc_grpc/linuxcnc/machine_health.hpp"

#include <cstdio>

#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"

namespace linuxcnc::server {

void MachineHealthCache::observe(const NmlStatusSnapshot& snapshot,
                                 Clock::time_point now) {
  std::lock_guard lock(mutex_);
  latest_.available = true;
  latest_.task_state = snapshot.task_state;
  latest_.interp_state = snapshot.interp_state;
  latest_.in_position = snapshot.motion_stat.traj.in_position;
  latest_.ini_filename = snapshot.ini_filename;
  observed_at_ = now;
}

void MachineHealthCache::mark_unavailable() {
  std::lock_guard lock(mutex_);
  latest_ = MachineHealth{};
}

MachineHealth MachineHealthCache::read(Clock::time_point now) const {
  std::lock_guard lock(mutex_);
  MachineHealth result = latest_;
  if (!result.available) return result;
  result.age =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - observed_at_);
  if (result.age > stale_after_) return MachineHealth{};
  return result;
}

namespace {

void append_json_string(std::string* out, const std::string& value) {
  out->push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      out->push_back('\\');
      out->push_back(character);
    } else if (byte < 0x20) {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      out->append(escaped);
    } else {
      out->push_back(character);
    }
  }
  out->push_back('"');
}

}  // namespace

std::string format_health_json(bool serving, const MachineHealth& machine) {
  std::string out = "{\"version\":1,\"serving\":";
  out += serving ? "true" : "false";
  out += ",\"machine\":{\"available\":";
  out += machine.available ? "true" : "false";
  if (machine.available) {
    out += ",\"age_ms\":" + std::to_string(machine.age.count());
    out += ",\"task_state\":" + std::to_string(machine.task_state);
    out += ",\"interp_state\":" + std::to_string(machine.interp_state);
    out += ",\"in_position\":";
    out += machine.in_position ? "true" : "false";
    out += ",\"ini\":";
    append_json_string(&out, machine.ini_filename);
  }
  out += "}}";
  return out;
}

}  // namespace linuxcnc::server

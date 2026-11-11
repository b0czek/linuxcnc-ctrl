#include <hal.h>
#include <rtapi.h>
#include <scope_shm.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../src/scope/delta_count.hpp"
#include "../src/scope/shm_abi.h"
#include "linuxcnc_grpc/scope/controller.hpp"

using namespace linuxcnc::server;

int main() {
  static_assert(sizeof(linuxcnc_scope_shm_control_t) ==
                sizeof(scope_shm_control_t));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_offset) ==
                offsetof(scope_shm_control_t, data_offset));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_type) ==
                offsetof(scope_shm_control_t, data_type));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_len) ==
                offsetof(scope_shm_control_t, data_len));

  // scope_rt first grows the record and then slides it; a snapshot pair can
  // straddle the moment it fills, and every sample on both sides counts.
  constexpr int cells_per_sample = 16;
  constexpr int buffer_cells = 100 * cells_per_sample;
  constexpr auto recorded = [](ScopeRecordPosition previous,
                               ScopeRecordPosition current) {
    return scope_new_sample_count(previous, current, cells_per_sample,
                                  buffer_cells);
  };
  static_assert(recorded({0, 40}, {0, 40}) == 0);
  static_assert(recorded({0, 40}, {0, 50}) == 10);
  static_assert(recorded({0, 90}, {10 * cells_per_sample, 99}) == 19);
  static_assert(
      recorded({10 * cells_per_sample, 99}, {25 * cells_per_sample, 99}) == 15);
  static_assert(
      recorded({95 * cells_per_sample, 99}, {5 * cells_per_sample, 99}) == 10);
  static_assert(!recorded({0, 50}, {0, 40}).has_value());
  static_assert(!recorded({0, 99}, {8, 99}).has_value());

  ScopeFrameQueue queue;
  int notifications = 0;
  queue.set_listener([&] { ++notifications; });
  assert(!queue.acquire(""));
  assert(queue.acquire("inspector-a"));
  assert(!queue.acquire("inspector-b"));

  ScopeFrame first;
  first.kind = ScopeFrameKind::Capture;
  first.payload = ScopeCapture{};
  assert(queue.publish(first));
  assert(notifications == 1);
  const auto delivered = queue.next("inspector-a");
  assert(delivered.has_value());
  assert(delivered->generation == 1);
  assert(std::holds_alternative<ScopeCapture>(delivered->payload));
  assert(!queue.next("inspector-a").has_value());

  ScopeFrame second;
  second.kind = ScopeFrameKind::Roll;
  second.payload = ScopeCaptureDelta{};
  assert(queue.publish(second));
  ScopeFrame third;
  third.kind = ScopeFrameKind::Roll;
  third.payload = ScopeCaptureDelta{};
  assert(queue.publish(third));
  assert(notifications == 3);

  const auto next = queue.acknowledge("inspector-a", delivered->generation);
  assert(next.has_value());
  assert(next->generation == 3);
  assert(next->skipped_frames == 1);
  assert(queue.skipped_frames() == 1);
  assert(!queue.acknowledge("inspector-a", delivered->generation).has_value());
  assert(!queue.acknowledge("inspector-a", next->generation).has_value());

  queue.release("inspector-a");
  assert(!queue.acquired());
  assert(queue.acquire("inspector-b"));
  return 0;
}

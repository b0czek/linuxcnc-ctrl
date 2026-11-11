#include <hal.h>
#include <rtapi.h>
#include <scope_shm.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "../src/scope/delta_count.hpp"
#include "../src/scope/sample_codec.hpp"
#include "../src/scope/shm_abi.h"
#include "linuxcnc_grpc/scope/controller.hpp"

using namespace linuxcnc::server;

namespace {

// Copies a cell written through upstream's union into ours, as the
// controller sees scope_rt's buffer.
linuxcnc_scope_data_t as_cell(const scope_data_t& upstream) {
  linuxcnc_scope_data_t cell;
  std::memcpy(&cell, &upstream, sizeof(cell));
  return cell;
}

int trigger_error(hal_type_t type, double level) {
  try {
    scope_codec::validate_trigger_level(type, level);
  } catch (const ScopeControllerError& error) {
    return error.code();
  }
  return 0;
}

void check_sample_codec() {
  using scope_codec::decode;

  // scope_rt writes only the member matching the channel type; the rest of
  // the cell keeps whatever an earlier sample left there.
  scope_data_t upstream;
  std::memset(&upstream, 0xA5, sizeof(upstream));
  upstream.b = true;
  assert(decode(as_cell(upstream), HAL_BOOL) == 1.0);
  upstream.b = false;
  assert(decode(as_cell(upstream), HAL_BOOL) == 0.0);

  upstream.s = -5;
  assert(decode(as_cell(upstream), HAL_SINT) == -5.0);
  upstream.s = std::numeric_limits<rtapi_sint>::min();
  assert(decode(as_cell(upstream), HAL_SINT) == -9223372036854775808.0);
  upstream.s = (rtapi_sint{1} << 53) + 1;  // not representable: rounds
  assert(decode(as_cell(upstream), HAL_SINT) == 9007199254740992.0);

  upstream.u = (rtapi_uint{1} << 32) + 7;
  assert(decode(as_cell(upstream), HAL_UINT) == 4294967303.0);
  upstream.u = std::numeric_limits<rtapi_uint>::max();
  assert(decode(as_cell(upstream), HAL_UINT) == 18446744073709551616.0);

  upstream.r = -1.25;
  assert(decode(as_cell(upstream), HAL_REAL) == -1.25);
  assert(decode(as_cell(upstream), HAL_PORT) == 0.0);

  assert(scope_codec::sampleable(HAL_BOOL));
  assert(scope_codec::sampleable(HAL_REAL));
  assert(scope_codec::sampleable(HAL_SINT));
  assert(scope_codec::sampleable(HAL_UINT));
  assert(!scope_codec::sampleable(HAL_PORT));
  assert(!scope_codec::sampleable(HAL_TYPE_UNSPECIFIED));

  // Trigger levels are compared by scope_rt through upstream's union.
  linuxcnc_scope_data_t level;
  scope_data_t read_back;
  scope_codec::encode_trigger_level(&level, HAL_SINT, -3.5e12);
  std::memcpy(&read_back, &level, sizeof(level));
  assert(read_back.s == -3500000000000);
  scope_codec::encode_trigger_level(&level, HAL_UINT, 6e9);
  std::memcpy(&read_back, &level, sizeof(level));
  assert(read_back.u == 6000000000U);
  scope_codec::encode_trigger_level(&level, HAL_REAL, 0.5);
  std::memcpy(&read_back, &level, sizeof(level));
  assert(read_back.r == 0.5);
  scope_codec::encode_trigger_level(&level, HAL_BOOL, 1.0);
  std::memcpy(&read_back, &level, sizeof(level));
  assert(read_back.b);

  assert(trigger_error(HAL_SINT, -9223372036854775808.0) == 0);
  assert(trigger_error(HAL_SINT, 9223372036854775808.0) == -ERANGE);
  assert(trigger_error(HAL_SINT, -1e19) == -ERANGE);
  assert(trigger_error(HAL_UINT, 18446744073709549568.0) == 0);
  assert(trigger_error(HAL_UINT, 18446744073709551616.0) == -ERANGE);
  assert(trigger_error(HAL_UINT, -1.0) == -ERANGE);
  assert(trigger_error(HAL_SINT, std::numeric_limits<double>::quiet_NaN()) ==
         -ERANGE);
  assert(trigger_error(HAL_REAL, 1e300) == 0);
}

}  // namespace

int main() {
  static_assert(sizeof(linuxcnc_scope_data_t) == sizeof(scope_data_t));
  static_assert(sizeof(linuxcnc_scope_shm_control_t) ==
                sizeof(scope_shm_control_t));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, trig_level) ==
                offsetof(scope_shm_control_t, trig_level));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, state) ==
                offsetof(scope_shm_control_t, state));
  static_assert(
      std::is_same_v<decltype(linuxcnc_scope_shm_control_t::data_offset),
                     decltype(scope_shm_control_t::data_offset)>);
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_offset) ==
                offsetof(scope_shm_control_t, data_offset));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_type) ==
                offsetof(scope_shm_control_t, data_type));
  static_assert(offsetof(linuxcnc_scope_shm_control_t, data_len) ==
                offsetof(scope_shm_control_t, data_len));
  // scope_rt's header rounding: (sizeof(scope_shm_control_t) + 7) & ~7.
  static_assert(LINUXCNC_SCOPE_BUFFER_OFFSET ==
                ((sizeof(scope_shm_control_t) + 7) & ~std::size_t{7}));
  check_sample_codec();

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

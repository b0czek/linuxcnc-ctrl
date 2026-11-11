#pragma once

// Conversion between scope_rt's per-sample cells and the controller's
// double-valued channels. Kept separate from controller.cc so the contract
// test can check it against upstream's scope_data_t.
//
// LinuxCNC 2.10's scope_rt records one 8-byte cell per channel and sample,
// writing only the union member that matches the channel's HAL type:
//
//   HAL_BOOL  rtapi_bool  (1 byte; the other 7 bytes keep stale data)
//   HAL_SINT  rtapi_sint  (int64_t)
//   HAL_UINT  rtapi_uint  (uint64_t)
//   HAL_REAL  rtapi_real  (double)
//
// Samples leave the controller as doubles. BOOL and REAL are exact. SINT and
// UINT are exact while their magnitude is at most 2^53; larger values are
// rounded to the nearest double (relative error below 2^-53), because the
// scope wire format carries only doubles.

#ifndef ULAPI
#error "scope/sample_codec.hpp requires ULAPI"
#endif

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "linuxcnc_grpc/scope/controller.hpp"
#include "scope/shm_abi.h"

namespace linuxcnc::server::scope_codec {

static_assert(sizeof(linuxcnc_scope_data_t) == 8);
static_assert(sizeof(rtapi_sint) == 8 && std::is_signed_v<rtapi_sint>);
static_assert(sizeof(rtapi_uint) == 8 && std::is_unsigned_v<rtapi_uint>);
static_assert(sizeof(rtapi_real) == 8);

// Whether scope_rt can sample a HAL item of this type.
constexpr bool sampleable(hal_type_t type) {
  return type == HAL_BOOL || type == HAL_REAL || type == HAL_SINT ||
         type == HAL_UINT;
}

namespace detail {
template <typename T>
T load(const linuxcnc_scope_data_t& cell) {
  // The cell lives in memory written by another process; copy its bytes
  // rather than read a union member that may not be the one last written.
  T value;
  std::memcpy(&value, &cell, sizeof(T));
  return value;
}
}  // namespace detail

inline double decode(const linuxcnc_scope_data_t& cell, hal_type_t type) {
  switch (type) {
    case HAL_BOOL:
      // Only the first byte is written for a bool channel.
      return detail::load<unsigned char>(cell) != 0 ? 1.0 : 0.0;
    case HAL_REAL:
      return detail::load<rtapi_real>(cell);
    case HAL_SINT:
      return static_cast<double>(detail::load<rtapi_sint>(cell));
    case HAL_UINT:
      return static_cast<double>(detail::load<rtapi_uint>(cell));
    default:
      return 0.0;
  }
}

// 2^63 and 2^64: the first doubles outside the int64_t and uint64_t ranges.
inline constexpr double kSintLimit = 9223372036854775808.0;
inline constexpr double kUintLimit = 18446744073709551616.0;

// Throws when `level` cannot be stored as a trigger level for `type`.
inline void validate_trigger_level(hal_type_t type, double level) {
  if (type == HAL_SINT &&
      (!std::isfinite(level) || level < -kSintLimit || level >= kSintLimit)) {
    throw ScopeControllerError("S64 trigger level is out of range", -ERANGE);
  }
  if (type == HAL_UINT &&
      (!std::isfinite(level) || level < 0.0 || level >= kUintLimit)) {
    throw ScopeControllerError("U64 trigger level is out of range", -ERANGE);
  }
}

// Stores a validated trigger level. Integer levels are truncated toward
// zero; scope_rt triggers when the sampled value is greater than the level.
// Bool channels trigger on the value itself and ignore the level.
inline void encode_trigger_level(linuxcnc_scope_data_t* cell, hal_type_t type,
                                 double level) {
  *cell = linuxcnc_scope_data_t{};
  switch (type) {
    case HAL_BOOL:
      cell->b = level != 0.0;
      break;
    case HAL_SINT:
      cell->s = static_cast<rtapi_sint>(level);
      break;
    case HAL_UINT:
      cell->u = static_cast<rtapi_uint>(level);
      break;
    default:
      cell->r = level;
      break;
  }
}

}  // namespace linuxcnc::server::scope_codec

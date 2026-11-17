#pragma once

// Conversion between the adapter's typed values and HAL's 64-bit storage.
// Kept separate from adapter.cc so a unit test can check it without a running
// HAL.
//
// LinuxCNC HAL (2.10+) stores integers as 64-bit HAL_SINT/HAL_UINT only.
// Items of owned components may still be declared S32 or U32: they are 64-bit
// HAL items whose owner reads them clamped to the 32-bit range. A value
// written from outside must therefore fit the declared range (fits_declared),
// or the owner would silently see a different value than the writer was told
// it wrote.

#ifndef ULAPI
#error "hal/value_codec.hpp requires ULAPI"
#endif

#include <hal.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

#include "linuxcnc_grpc/hal/adapter.hpp"

namespace linuxcnc::server::hal_codec {

static_assert(sizeof(rtapi_sint) == 8 && std::is_signed_v<rtapi_sint>);
static_assert(sizeof(rtapi_uint) == 8 && std::is_unsigned_v<rtapi_uint>);

// Converts a client value for an item of HAL type `type`. Integer values must
// match the item's signedness; 32-bit values are widened to HAL's 64-bit
// storage, which cannot go out of range. Cross-signedness writes are refused,
// as mismatched types were before.
inline std::optional<hal_query_value_u> to_hal(hal_type_t type,
                                               const HalAdapterValue& value) {
  hal_query_value_u result{};
  switch (type) {
    case HAL_BOOL:
      if (const auto* typed = std::get_if<bool>(&value)) {
        result.b = *typed;
        return result;
      }
      break;
    case HAL_REAL:
      if (const auto* typed = std::get_if<double>(&value)) {
        result.r = *typed;
        return result;
      }
      break;
    case HAL_SINT:
      if (const auto* typed = std::get_if<std::int64_t>(&value)) {
        result.s = *typed;
        return result;
      }
      if (const auto* typed = std::get_if<std::int32_t>(&value)) {
        result.s = *typed;
        return result;
      }
      break;
    case HAL_UINT:
      if (const auto* typed = std::get_if<std::uint64_t>(&value)) {
        result.u = *typed;
        return result;
      }
      if (const auto* typed = std::get_if<std::uint32_t>(&value)) {
        result.u = *typed;
        return result;
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

// Whether `value`, stored in a HAL item of type `type`, reaches an owner that
// declared the item `declared` unchanged. Only S32 and U32 declarations
// narrow; every value fits the other types.
inline bool fits_declared(HalAdapterType declared, hal_type_t type,
                          const hal_query_value_u& value) {
  if (declared == HalAdapterType::S32 && type == HAL_SINT)
    return value.s >= std::numeric_limits<std::int32_t>::min() &&
           value.s <= std::numeric_limits<std::int32_t>::max();
  if (declared == HalAdapterType::U32 && type == HAL_UINT)
    return value.u <= std::numeric_limits<std::uint32_t>::max();
  return true;
}

// Reads an owned item through its handle. Items declared with a 32-bit type
// keep that type for their owner; HAL stores 64 bits, so a value outside the
// 32-bit range (which only a linked 64-bit writer pin can still produce) is
// clamped.
inline std::optional<HalAdapterValue> read_owned(HalAdapterType type,
                                                 const hal_refs_u& ref) {
  switch (type) {
    case HalAdapterType::Bit:
      if (!ref.b) return std::nullopt;
      return static_cast<bool>(hal_get_bool(ref.b));
    case HalAdapterType::Float:
      if (!ref.r) return std::nullopt;
      return static_cast<double>(hal_get_real(ref.r));
    case HalAdapterType::S32:
      if (!ref.s) return std::nullopt;
      return static_cast<std::int32_t>(hal_get_si32_clamped(ref.s));
    case HalAdapterType::U32:
      if (!ref.u) return std::nullopt;
      return static_cast<std::uint32_t>(hal_get_ui32_clamped(ref.u));
    case HalAdapterType::S64:
      if (!ref.s) return std::nullopt;
      return static_cast<std::int64_t>(hal_get_sint(ref.s));
    case HalAdapterType::U64:
      if (!ref.u) return std::nullopt;
      return static_cast<std::uint64_t>(hal_get_uint(ref.u));
  }
  return std::nullopt;
}

}  // namespace linuxcnc::server::hal_codec

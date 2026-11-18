#include <hal.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <variant>

#include "../src/hal/narrow_item_registry.hpp"
#include "../src/hal/value_codec.hpp"
#include "linuxcnc_grpc/hal/adapter.hpp"

using namespace linuxcnc::server;

namespace {

constexpr auto kS32Min = std::numeric_limits<std::int32_t>::min();
constexpr auto kS32Max = std::numeric_limits<std::int32_t>::max();
constexpr auto kU32Max = std::numeric_limits<std::uint32_t>::max();

hal_query_value_u sint(rtapi_sint value) {
  hal_query_value_u result{};
  result.s = value;
  return result;
}

hal_query_value_u uint(rtapi_uint value) {
  hal_query_value_u result{};
  result.u = value;
  return result;
}

// to_hal() for a conversion that must succeed.
hal_query_value_u converted(hal_type_t type, const HalAdapterValue& value) {
  const auto result = hal_codec::to_hal(type, value);
  if (!result) std::abort();
  return *result;
}

// The value of a read that must succeed with type T.
template <typename T>
T owned(const std::optional<HalAdapterValue>& value) {
  if (!value || !std::holds_alternative<T>(*value)) std::abort();
  return std::get<T>(*value);
}

void check_to_hal() {
  using hal_codec::to_hal;
  // Exact types.
  assert(converted(HAL_BOOL, HalAdapterValue{true}).b);
  assert(converted(HAL_REAL, HalAdapterValue{2.5}).r == 2.5);
  assert(converted(HAL_SINT, HalAdapterValue{std::int64_t{-30000000000}}).s ==
         -30000000000);
  assert(converted(HAL_UINT, HalAdapterValue{std::uint64_t{30000000000}}).u ==
         30000000000U);

  // 32-bit values widen to HAL's 64-bit storage, keeping their value.
  assert(converted(HAL_SINT, HalAdapterValue{kS32Min}).s == kS32Min);
  assert(converted(HAL_SINT, HalAdapterValue{std::int32_t{-1}}).s == -1);
  assert(converted(HAL_UINT, HalAdapterValue{kU32Max}).u == kU32Max);

  // Signedness must match: no implicit reinterpretation.
  assert(!to_hal(HAL_SINT, HalAdapterValue{std::uint64_t{1}}));
  assert(!to_hal(HAL_SINT, HalAdapterValue{std::uint32_t{1}}));
  assert(!to_hal(HAL_UINT, HalAdapterValue{std::int64_t{1}}));
  assert(!to_hal(HAL_UINT, HalAdapterValue{std::int32_t{1}}));

  // Other mismatches.
  assert(!to_hal(HAL_BOOL, HalAdapterValue{std::int64_t{1}}));
  assert(!to_hal(HAL_REAL, HalAdapterValue{std::int64_t{1}}));
  assert(!to_hal(HAL_SINT, HalAdapterValue{1.0}));
  assert(!to_hal(HAL_UINT, HalAdapterValue{true}));
  assert(!to_hal(HAL_PORT, HalAdapterValue{std::uint64_t{1}}));
}

void check_fits_declared() {
  using hal_codec::fits_declared;
  const auto s32 = HalAdapterType::S32;
  const auto u32 = HalAdapterType::U32;
  assert(fits_declared(s32, HAL_SINT, sint(0)));
  assert(fits_declared(s32, HAL_SINT, sint(kS32Min)));
  assert(fits_declared(s32, HAL_SINT, sint(kS32Max)));
  assert(!fits_declared(s32, HAL_SINT, sint(rtapi_sint{kS32Max} + 1)));
  assert(!fits_declared(s32, HAL_SINT, sint(rtapi_sint{kS32Min} - 1)));
  assert(!fits_declared(s32, HAL_SINT, sint(30000000000)));
  assert(fits_declared(u32, HAL_UINT, uint(0)));
  assert(fits_declared(u32, HAL_UINT, uint(kU32Max)));
  assert(!fits_declared(u32, HAL_UINT, uint(rtapi_uint{kU32Max} + 1)));
  assert(!fits_declared(u32, HAL_UINT, uint(30000000000U)));

  // 64-bit and non-integer declarations take every value.
  assert(fits_declared(HalAdapterType::S64, HAL_SINT,
                       sint(std::numeric_limits<rtapi_sint>::min())));
  assert(fits_declared(HalAdapterType::U64, HAL_UINT,
                       uint(std::numeric_limits<rtapi_uint>::max())));
  assert(fits_declared(HalAdapterType::Bit, HAL_BOOL, hal_query_value_u{}));
  assert(fits_declared(HalAdapterType::Float, HAL_REAL, hal_query_value_u{}));
}

// read_owned() reads through HAL's inline accessors, which only dereference
// the handle; a local cell stands in for HAL shared memory.
union Cell {
  rtapi_sint s;
  rtapi_uint u;
};

std::optional<HalAdapterValue> read_sint(HalAdapterType type,
                                         rtapi_sint stored) {
  Cell cell{};
  cell.s = stored;
  hal_refs_u ref{};
  // NOLINTNEXTLINE(bugprone-casting-through-void)
  ref.s = static_cast<hal_sint_t>(static_cast<void*>(&cell));
  return hal_codec::read_owned(type, ref);
}

std::optional<HalAdapterValue> read_uint(HalAdapterType type,
                                         rtapi_uint stored) {
  Cell cell{};
  cell.u = stored;
  hal_refs_u ref{};
  // NOLINTNEXTLINE(bugprone-casting-through-void)
  ref.u = static_cast<hal_uint_t>(static_cast<void*>(&cell));
  return hal_codec::read_owned(type, ref);
}

void check_read_owned() {
  const auto s32 = HalAdapterType::S32;
  const auto u32 = HalAdapterType::U32;
  // In range: the declared 32-bit type with the exact value.
  assert(owned<std::int32_t>(read_sint(s32, -55)) == -55);
  assert(owned<std::int32_t>(read_sint(s32, kS32Min)) == kS32Min);
  assert(owned<std::uint32_t>(read_uint(u32, kU32Max)) == kU32Max);
  // Out of range (only a linked 64-bit writer can still produce this): the
  // owner sees the nearest 32-bit limit.
  assert(owned<std::int32_t>(read_sint(s32, 30000000000)) == kS32Max);
  assert(owned<std::int32_t>(read_sint(s32, -30000000000)) == kS32Min);
  assert(owned<std::uint32_t>(read_uint(u32, 30000000000U)) == kU32Max);
  // 64-bit declarations are exact.
  assert(owned<std::int64_t>(read_sint(HalAdapterType::S64, -30000000000)) ==
         -30000000000);
  assert(owned<std::uint64_t>(read_uint(HalAdapterType::U64, 30000000000U)) ==
         30000000000U);
  // A missing handle reads as nothing.
  assert(!hal_codec::read_owned(s32, hal_refs_u{}));
  assert(!hal_codec::read_owned(HalAdapterType::U64, hal_refs_u{}));
}

// Unlike assert(), stays active when NDEBUG is defined.
void require(bool condition, const char* description) {
  if (condition) return;
  std::cerr << "narrow item registry check failed: " << description << "\n";
  std::abort();
}

constexpr int kOwner = 7;
constexpr int kOtherOwner = 8;
constexpr rtapi_sint kTooLarge = rtapi_sint{kS32Max} + 1;

bool accepts_sint(const NarrowItemRegistry& registry, bool pin,
                  const char* name, const char* alias, int owner,
                  rtapi_sint value) {
  return registry.accepts(pin, name, alias, owner, HAL_SINT, sint(value));
}

void check_narrow_registry_add_and_ranges() {
  NarrowItemRegistry registry;
  require(registry.empty(), "a new registry is empty");
  require(accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "an unregistered item takes any value");

  require(registry.add(true, "c.in", kOwner, HalAdapterType::S32),
          "first registration succeeds");
  require(!registry.empty(), "a registered item makes the registry non-empty");
  require(accepts_sint(registry, true, "c.in", nullptr, kOwner, kS32Max),
          "the S32 maximum fits");
  require(accepts_sint(registry, true, "c.in", nullptr, kOwner, kS32Min),
          "the S32 minimum fits");
  require(!accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "a value above the S32 range is refused");
  require(!accepts_sint(registry, true, "c.in", nullptr, kOwner,
                        rtapi_sint{kS32Min} - 1),
          "a value below the S32 range is refused");

  require(registry.add(true, "c.count", kOwner, HalAdapterType::U32),
          "a second item registers");
  require(registry.accepts(true, "c.count", nullptr, kOwner, HAL_UINT,
                           uint(kU32Max)),
          "the U32 maximum fits");
  require(!registry.accepts(true, "c.count", nullptr, kOwner, HAL_UINT,
                            uint(rtapi_uint{kU32Max} + 1)),
          "a value above the U32 range is refused");

  // Pins and parameters are separate HAL namespaces.
  require(accepts_sint(registry, false, "c.in", nullptr, kOwner, kTooLarge),
          "a parameter does not match a pin of the same name");
  require(registry.add(false, "c.in", kOwner, HalAdapterType::S32),
          "a parameter may share a pin's name");
  require(!accepts_sint(registry, false, "c.in", nullptr, kOwner, kTooLarge),
          "a registered parameter is range checked");

  // The first registration stays when HAL would refuse the duplicate name.
  require(!registry.add(true, "c.in", kOtherOwner, HalAdapterType::U32),
          "a duplicate name is not registered");
  require(!accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "a refused duplicate leaves the original entry");
  require(accepts_sint(registry, true, "c.in", nullptr, kOtherOwner, kTooLarge),
          "a refused duplicate does not register its owner");

  require(NarrowItemRegistry::is_narrow(HalAdapterType::S32) &&
              NarrowItemRegistry::is_narrow(HalAdapterType::U32),
          "32-bit types are narrow");
  require(!NarrowItemRegistry::is_narrow(HalAdapterType::S64) &&
              !NarrowItemRegistry::is_narrow(HalAdapterType::U64) &&
              !NarrowItemRegistry::is_narrow(HalAdapterType::Bit) &&
              !NarrowItemRegistry::is_narrow(HalAdapterType::Float),
          "other types are not narrow");
}

void check_narrow_registry_owner_mismatch() {
  NarrowItemRegistry registry;
  require(registry.add(true, "c.in", kOwner, HalAdapterType::S32),
          "registration succeeds");
  // A foreign item that reuses the name is not the owned item.
  require(accepts_sint(registry, true, "c.in", nullptr, kOtherOwner, kTooLarge),
          "another component's item is not range checked");
  registry.remove(true, "c.in", kOtherOwner);
  require(!registry.empty(), "removal by another owner removes nothing");
  require(!accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "the entry survives removal by another owner");
  registry.remove(false, "c.in", kOwner);
  require(!accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "removing a parameter leaves the pin of the same name");
}

void check_narrow_registry_alias() {
  NarrowItemRegistry registry;
  require(registry.add(true, "c.in", kOwner, HalAdapterType::S32),
          "registration succeeds");
  // HAL reports an aliased item under its alias, with the original name as
  // the old name; the entry is keyed by the original name.
  require(!accepts_sint(registry, true, "alias", "c.in", kOwner, kTooLarge),
          "an aliased item is checked through its original name");
  require(accepts_sint(registry, true, "alias", "c.in", kOwner, kS32Max),
          "an aliased item takes values in range");
  require(accepts_sint(registry, true, "alias", "c.in", kOtherOwner, kTooLarge),
          "the alias candidate still requires the owner to match");
  require(accepts_sint(registry, true, "alias", "other", kOwner, kTooLarge),
          "an unrelated original name is not checked");
}

void check_narrow_registry_remove_and_recreate() {
  NarrowItemRegistry registry;
  require(registry.add(true, "c.in", kOwner, HalAdapterType::S32),
          "registration succeeds");
  registry.remove(true, "c.in", kOwner);
  require(registry.empty(), "removing the last entry empties the registry");
  require(accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "a removed item is no longer checked");

  // A component re-created under the same name gets a new id and may declare
  // a different type; the check must follow the new declaration.
  require(registry.add(true, "c.in", kOtherOwner, HalAdapterType::U32),
          "the name registers again after removal");
  require(accepts_sint(registry, true, "c.in", nullptr, kOwner, kTooLarge),
          "the old owner no longer matches");
  require(!registry.accepts(true, "c.in", nullptr, kOtherOwner, HAL_UINT,
                            uint(rtapi_uint{kU32Max} + 1)),
          "the new declaration is enforced");
  require(registry.accepts(true, "c.in", nullptr, kOtherOwner, HAL_UINT,
                           uint(kU32Max)),
          "the new declaration's range applies");
}

}  // namespace

int main() {
  check_to_hal();
  check_fits_declared();
  check_read_owned();
  check_narrow_registry_add_and_ranges();
  check_narrow_registry_owner_mismatch();
  check_narrow_registry_alias();
  check_narrow_registry_remove_and_recreate();
  return 0;
}

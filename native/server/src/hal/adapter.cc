#ifndef ULAPI
#define ULAPI
#endif

#include "linuxcnc_grpc/hal/adapter.hpp"

#include <hal.h>
#include <rtapi.h>

// All HAL access goes through LinuxCNC's public API: the query API
// (hal_list_*, hal_get_*, hal_set_*, hal_comp_by_name) for inspecting and
// writing items by name, and the typed getters/setters for the pins and
// parameters of components this adapter owns.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "hal/query.hpp"

namespace linuxcnc::server {
namespace {

// Thread "running" detection (see LinuxCncHalAdapter::Impl::threads_running).
// A running thread advances its threadbeat pin once per period; a beat that
// stays put for twice the period plus this slack means threads are stopped.
// The slack absorbs wake-up latency, which is large without a realtime
// kernel.
constexpr auto kThreadBeatSlack = std::chrono::milliseconds(20);
// Longest time topology() waits for a beat when earlier observations cannot
// decide, and the interval between reads while it waits.
constexpr auto kThreadBeatMaxWait = std::chrono::milliseconds(100);
constexpr auto kThreadBeatPollInterval = std::chrono::milliseconds(1);

[[noreturn]] void throw_hal_error(const std::string& operation, int code) {
  // The query API reports -EFAULT only when HAL shared memory is not mapped.
  if (code == -EFAULT) throw HalAdapterError("HAL is not initialized", -ENODEV);
  throw HalAdapterError(operation + " failed", code);
}

int checked(int result, const char* operation) {
  if (result < 0) throw_hal_error(operation, result);
  return result;
}

// Runs `fn` while holding HAL's mutex, so all queries it makes observe one
// consistent HAL state, as the previous direct shared-memory walk did.
template <typename Fn>
void run_locked(Fn&& fn) {
  checked(hal_query::with_lock(std::forward<Fn>(fn)), "HAL lock");
}

hal_type_t native_type(HalAdapterType type) {
  switch (type) {
    case HalAdapterType::Bit:
      return HAL_BOOL;
    case HalAdapterType::Float:
      return HAL_REAL;
    case HalAdapterType::S32:
    case HalAdapterType::S64:
      return HAL_SINT;
    case HalAdapterType::U32:
    case HalAdapterType::U64:
      return HAL_UINT;
  }
  return HAL_TYPE_UNSPECIFIED;
}

// HAL integers are 64-bit only, so HAL items are always reported as S64/U64.
std::optional<HalAdapterType> adapter_type(hal_type_t type) {
  switch (type) {
    case HAL_BOOL:
      return HalAdapterType::Bit;
    case HAL_REAL:
      return HalAdapterType::Float;
    case HAL_SINT:
      return HalAdapterType::S64;
    case HAL_UINT:
      return HalAdapterType::U64;
    default:
      // HAL_PORT is an implementation-specific internal type; it is not part
      // of the scalar values exposed by the gRPC HAL oneof.
      return std::nullopt;
  }
}

HalAdapterPinDirection pin_direction(hal_pdir_t direction) {
  if (direction == HAL_OUT) return HalAdapterPinDirection::Out;
  if (direction == HAL_IO) return HalAdapterPinDirection::Io;
  return HalAdapterPinDirection::In;
}

HalAdapterParamDirection param_direction(hal_pdir_t direction) {
  return direction == HAL_RW ? HalAdapterParamDirection::ReadWrite
                             : HalAdapterParamDirection::ReadOnly;
}

HalAdapterComponentKind component_kind(hal_comp_type_t type) {
  switch (type) {
    case HAL_COMP_TYPE_USER:
      return HalAdapterComponentKind::User;
    case HAL_COMP_TYPE_REALTIME:
      return HalAdapterComponentKind::Realtime;
    case HAL_COMP_TYPE_OTHER:
      return HalAdapterComponentKind::Other;
    default:
      return HalAdapterComponentKind::Unknown;
  }
}

bool same_type(HalAdapterType type, const HalAdapterValue& value) {
  switch (type) {
    case HalAdapterType::Bit:
      return std::holds_alternative<bool>(value);
    case HalAdapterType::Float:
      return std::holds_alternative<double>(value);
    case HalAdapterType::S32:
      return std::holds_alternative<std::int32_t>(value);
    case HalAdapterType::U32:
      return std::holds_alternative<std::uint32_t>(value);
    case HalAdapterType::S64:
      return std::holds_alternative<std::int64_t>(value);
    case HalAdapterType::U64:
      return std::holds_alternative<std::uint64_t>(value);
  }
  return false;
}

std::int32_t clamp_s32(rtapi_sint value) {
  return static_cast<std::int32_t>(
      std::clamp<rtapi_sint>(value, std::numeric_limits<std::int32_t>::min(),
                             std::numeric_limits<std::int32_t>::max()));
}

HalAdapterValue from_hal(hal_type_t type, const hal_query_value_u& value) {
  switch (type) {
    case HAL_BOOL:
      return static_cast<bool>(value.b);
    case HAL_REAL:
      return static_cast<double>(value.r);
    case HAL_SINT:
      return static_cast<std::int64_t>(value.s);
    case HAL_UINT:
      return static_cast<std::uint64_t>(value.u);
    default:
      throw HalAdapterError("Unsupported HAL scalar type", -EINVAL);
  }
}

// Converts a client value for an item of HAL type `type`. Integer values must
// match the item's signedness; 32-bit values are widened to HAL's 64-bit
// storage, which cannot go out of range. Cross-signedness writes are refused,
// as mismatched types were before.
std::optional<hal_query_value_u> to_hal(hal_type_t type,
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

// Looks up a pin (HAL_QTYPE_PIN) or parameter (HAL_QTYPE_PARAM) by name.
// The returned query's strings are valid only while HAL's mutex is held.
std::optional<hal_query_t> find_pp(const std::string& name, hal_qtype_t qtype) {
  if (name.empty() || name.size() > HAL_NAME_LEN) return std::nullopt;
  hal_query_t query{};
  query.name = name.c_str();
  query.qtype = qtype;
  const int result = hal_get_p(&query, nullptr, nullptr);
  if (result == -ENOENT) return std::nullopt;
  checked(result, "hal_get_p");
  return query;
}

std::optional<hal_query_t> find_signal(const std::string& name) {
  if (name.empty() || name.size() > HAL_NAME_LEN) return std::nullopt;
  hal_query_t query{};
  query.name = name.c_str();
  const int result = hal_get_s(&query, nullptr, nullptr);
  if (result == -ENOENT) return std::nullopt;
  checked(result, "hal_get_s");
  return query;
}

// Reads a signed statistics item such as <function>.time (pin) or
// <thread>.tmax (parameter). HAL writes them with 32-bit setters.
std::optional<std::int32_t> statistic_value(const std::string& name,
                                            hal_qtype_t qtype) {
  const auto query = find_pp(name, qtype);
  if (!query || query->pp.type != HAL_SINT) return std::nullopt;
  return clamp_s32(query->pp.value.s);
}

std::optional<bool> statistic_flag(const std::string& name, hal_qtype_t qtype) {
  const auto query = find_pp(name, qtype);
  if (!query || query->pp.type != HAL_BOOL) return std::nullopt;
  return static_cast<bool>(query->pp.value.b);
}

hal_qtype_t pp_qtype(HalAdapterItemKind kind) {
  return kind == HalAdapterItemKind::Param ? HAL_QTYPE_PARAM : HAL_QTYPE_PIN;
}

// Must run under run_locked().
std::optional<HalAdapterValue> read_unlocked(
    const HalAdapterReference& reference) {
  hal_type_t type = HAL_TYPE_UNINITIALIZED;
  hal_query_value_u value{};
  if (reference.kind == HalAdapterItemKind::Signal) {
    const auto query = find_signal(reference.name);
    if (!query) return std::nullopt;
    type = query->sig.type;
    value = query->sig.value;
  } else {
    const auto query = find_pp(reference.name, pp_qtype(reference.kind));
    if (!query) return std::nullopt;
    type = query->pp.type;
    value = query->pp.value;
  }
  if (!adapter_type(type)) return std::nullopt;
  return from_hal(type, value);
}

struct PreparedWrite {
  const HalAdapterReference* reference = nullptr;
  hal_type_t type = HAL_TYPE_UNINITIALIZED;
  hal_query_value_u value{};
};

// Checks that `reference` exists and accepts `value` from outside HAL. Must
// run under run_locked() together with apply_unlocked(). The rules are those
// of the previous direct implementation: unlinked non-output pins, HAL_RW
// parameters and signals without a writer pin.
std::optional<PreparedWrite> prepare_unlocked(
    const HalAdapterReference& reference, const HalAdapterValue& value) {
  PreparedWrite prepared;
  prepared.reference = &reference;
  try {
    if (reference.kind == HalAdapterItemKind::Signal) {
      const auto query = find_signal(reference.name);
      if (!query || query->sig.writers > 0) return std::nullopt;
      prepared.type = query->sig.type;
    } else {
      const auto query = find_pp(reference.name, pp_qtype(reference.kind));
      if (!query) return std::nullopt;
      if (reference.kind == HalAdapterItemKind::Pin &&
          (query->pp.dir == HAL_OUT || query->pp.signal != nullptr))
        return std::nullopt;
      if (reference.kind == HalAdapterItemKind::Param &&
          query->pp.dir != HAL_RW)
        return std::nullopt;
      prepared.type = query->pp.type;
    }
  } catch (const HalAdapterError& error) {
    // An item without a data target (-EIO) is not writable, as before.
    if (error.code() == -EIO) return std::nullopt;
    throw;
  }
  const auto converted = to_hal(prepared.type, value);
  if (!converted) return std::nullopt;
  prepared.value = *converted;
  return prepared;
}

bool apply_unlocked(const PreparedWrite& prepared) {
  const auto& reference = *prepared.reference;
  hal_query_t query{};
  query.name = reference.name.c_str();
  int result = 0;
  if (reference.kind == HalAdapterItemKind::Signal) {
    query.sig.type = prepared.type;
    query.sig.value = prepared.value;
    result = hal_set_s(&query, nullptr, nullptr);
  } else {
    query.qtype = pp_qtype(reference.kind);
    query.pp.type = prepared.type;
    query.pp.value = prepared.value;
    result = hal_set_p(&query, nullptr, nullptr);
  }
  if (result == -EFAULT) throw_hal_error("HAL write", result);
  return result == 0;
}

HalAdapterValue read_back_unlocked(const HalAdapterReference& reference) {
  auto value = read_unlocked(reference);
  if (!value)
    throw HalAdapterError("HAL item '" + reference.name + "' vanished",
                          -ENOENT);
  return *value;
}

std::string full_name(const std::string& prefix, const std::string& suffix) {
  if (suffix.empty()) return {};
  auto result = prefix.empty() ? suffix : prefix + "." + suffix;
  if (result.size() > HAL_NAME_LEN) return {};
  return result;
}

int create_pin(int component_id, hal_pdir_t direction, HalAdapterType type,
               hal_refs_u* ref, const char* name) {
  switch (type) {
    case HalAdapterType::Bit:
      return hal_pin_new_bool(component_id, direction, &ref->b, false, "%s",
                              name);
    case HalAdapterType::Float:
      return hal_pin_new_real(component_id, direction, &ref->r, 0.0, "%s",
                              name);
    case HalAdapterType::S32:
      return hal_pin_new_si32(component_id, direction, &ref->s, 0, "%s", name);
    case HalAdapterType::U32:
      return hal_pin_new_ui32(component_id, direction, &ref->u, 0, "%s", name);
    case HalAdapterType::S64:
      return hal_pin_new_sint(component_id, direction, &ref->s, 0, "%s", name);
    case HalAdapterType::U64:
      return hal_pin_new_uint(component_id, direction, &ref->u, 0, "%s", name);
  }
  return -EINVAL;
}

int create_param(int component_id, hal_pdir_t direction, HalAdapterType type,
                 hal_refs_u* ref, const char* name) {
  switch (type) {
    case HalAdapterType::Bit:
      return hal_param_new_bool(component_id, direction, &ref->b, false, "%s",
                                name);
    case HalAdapterType::Float:
      return hal_param_new_real(component_id, direction, &ref->r, 0.0, "%s",
                                name);
    case HalAdapterType::S32:
      return hal_param_new_si32(component_id, direction, &ref->s, 0, "%s",
                                name);
    case HalAdapterType::U32:
      return hal_param_new_ui32(component_id, direction, &ref->u, 0, "%s",
                                name);
    case HalAdapterType::S64:
      return hal_param_new_sint(component_id, direction, &ref->s, 0, "%s",
                                name);
    case HalAdapterType::U64:
      return hal_param_new_uint(component_id, direction, &ref->u, 0, "%s",
                                name);
  }
  return -EINVAL;
}

// Reads an owned item through its handle. Items declared with a 32-bit type
// keep that type for their owner; HAL stores 64 bits, so a value outside the
// 32-bit range (set from outside, or by a linked 64-bit writer) is clamped.
std::optional<HalAdapterValue> read_owned(HalAdapterType type,
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

bool write_owned(HalAdapterType type, const hal_refs_u& ref,
                 const HalAdapterValue& value) {
  if (!same_type(type, value)) return false;
  switch (type) {
    case HalAdapterType::Bit:
      if (!ref.b) return false;
      hal_set_bool(ref.b, std::get<bool>(value));
      return true;
    case HalAdapterType::Float:
      if (!ref.r) return false;
      hal_set_real(ref.r, std::get<double>(value));
      return true;
    case HalAdapterType::S32:
      if (!ref.s) return false;
      hal_set_si32(ref.s, std::get<std::int32_t>(value));
      return true;
    case HalAdapterType::U32:
      if (!ref.u) return false;
      hal_set_ui32(ref.u, std::get<std::uint32_t>(value));
      return true;
    case HalAdapterType::S64:
      if (!ref.s) return false;
      hal_set_sint(ref.s, std::get<std::int64_t>(value));
      return true;
    case HalAdapterType::U64:
      if (!ref.u) return false;
      hal_set_uint(ref.u, std::get<std::uint64_t>(value));
      return true;
  }
  return false;
}

// Holds the pin and parameter handles of owned components. HAL requires the
// handle storage to come from hal_malloc(), which cannot be freed, so slots
// are recycled across components.
class HalAllocationSlab final {
 public:
  explicit HalAllocationSlab(std::size_t capacity) : capacity_(capacity) {
    const auto bytes = static_cast<long>(capacity * sizeof(hal_refs_u));
    slots_ = static_cast<hal_refs_u*>(hal_malloc(bytes));
    if (!slots_)
      throw HalAdapterError("HAL dynamic item slab allocation failed", -EBUSY);
    std::memset(slots_, 0, capacity * sizeof(hal_refs_u));
    free_.reserve(capacity);
    for (std::size_t index = capacity; index > 0; --index)
      free_.push_back(index - 1);
  }

  std::optional<std::size_t> acquire() {
    std::lock_guard lock(mutex_);
    if (free_.empty()) return std::nullopt;
    const auto index = free_.back();
    free_.pop_back();
    std::memset(slots_ + index, 0, sizeof(hal_refs_u));
    return index;
  }

  void release(std::size_t index) {
    std::lock_guard lock(mutex_);
    if (index >= capacity_) return;
    std::memset(slots_ + index, 0, sizeof(hal_refs_u));
    free_.push_back(index);
  }

  hal_refs_u* data(std::size_t index) { return slots_ + index; }

 private:
  const std::size_t capacity_;
  hal_refs_u* slots_ = nullptr;
  std::mutex mutex_;
  std::vector<std::size_t> free_;
};

}  // namespace

HalAdapterError::HalAdapterError(
    // NOLINTNEXTLINE(performance-unnecessary-value-param): public ABI contract
    std::string message, int code)
    : std::runtime_error(message), code_(code) {}

struct LinuxCncHalComponent::Impl {
  struct Item {
    std::string suffix;
    std::string full_name;
    HalAdapterType type = HalAdapterType::Bit;
    bool pin = false;
    hal_pdir_t direction = HAL_DIR_UNSPECIFIED;
    hal_refs_u* ref = nullptr;
    std::size_t slab_slot = 0;
  };

  int id = 0;
  std::string name;
  std::string prefix;
  bool ready = false;
  std::shared_ptr<HalAllocationSlab> slab;
  std::map<std::string, Item> items;

  void close() {
    if (id > 0) {
      hal_exit(id);
      id = 0;
    }
    if (slab) {
      for (const auto& [suffix, item] : items) {
        (void)suffix;
        slab->release(item.slab_slot);
      }
    }
    items.clear();
  }

  bool add(const std::string& suffix, HalAdapterType type, bool pin,
           hal_pdir_t direction) {
    if (id <= 0 || ready || suffix.empty() || items.find(suffix) != items.end())
      return false;
    const auto full = full_name(prefix, suffix);
    if (full.empty()) return false;
    const auto slot = slab->acquire();
    if (!slot)
      throw HalAdapterError("global HAL dynamic item quota reached", -EBUSY);
    auto [it, inserted] = items.emplace(
        suffix,
        Item{suffix, full, type, pin, direction, slab->data(*slot), *slot});
    if (!inserted) {
      slab->release(*slot);
      return false;
    }
    const auto& item = it->second;
    const int result =
        pin ? create_pin(id, direction, type, item.ref, item.full_name.c_str())
            : create_param(id, direction, type, item.ref,
                           item.full_name.c_str());
    if (result != 0) {
      items.erase(it);
      slab->release(*slot);
      throw HalAdapterError(std::string(pin ? "hal_pin_new" : "hal_param_new") +
                                " failed for '" + full + "'",
                            result);
    }
    return true;
  }
};

// Reads <thread>.threadbeat, which HAL increments once per cycle while
// threads run.
std::optional<rtapi_sint> read_thread_beat(const std::string& thread) {
  const auto beat = find_pp(thread + ".threadbeat", HAL_QTYPE_PIN);
  if (!beat || beat->pp.type != HAL_SINT) return std::nullopt;
  return beat->pp.value.s;
}

struct LinuxCncHalAdapter::Impl {
  using Clock = std::chrono::steady_clock;

  struct BeatState {
    rtapi_sint beat = 0;
    Clock::time_point since;    // first read that returned `beat`
    Clock::time_point read_at;  // latest read
  };

  int component_id = 0;
  std::string component_name;
  std::shared_ptr<HalAllocationSlab> slab;
  std::mutex beat_mutex;
  std::unordered_map<std::string, BeatState> beats;
  std::optional<bool> last_running;

  // HAL no longer exposes its global "threads running" flag through the
  // public API; hal_start_threads()/hal_stop_threads() change nothing else
  // a process can observe except each thread's <thread>.threadbeat pin,
  // which advances once per period only while threads run.
  //
  // `observed` holds the beats read in topology() at `read_at`. Compared
  // with the previous topology() call, a beat that changed within twice its
  // period plus kThreadBeatSlack proves threads are running, and a beat that
  // has stayed put for at least that long proves they are stopped. Without
  // such evidence (first call, a new thread, or a change seen only after a
  // longer gap) the beats are re-read every kThreadBeatPollInterval until
  // one moves (running) or twice the shortest period plus the slack has
  // passed (stopped). That wait is bounded by kThreadBeatMaxWait; when it
  // cannot decide (every thread period above about 40 ms) the previous
  // answer is kept, or, on the first call, a non-zero beat counts as
  // running.
  //
  // Cost: none when topology() is called again within the window; about one
  // period when threads run; twice the shortest period plus the slack (22 ms
  // with a 1 ms thread) on the first call while threads are stopped.
  bool threads_running(const std::vector<HalAdapterThreadInfo>& threads,
                       const std::vector<std::optional<rtapi_sint>>& observed,
                       Clock::time_point read_at) {
    std::lock_guard lock(beat_mutex);
    std::unordered_map<std::string, BeatState> next;
    bool running = false;
    bool stopped = false;
    bool any_beat = false;
    bool nonzero_beat = false;
    auto shortest_window = Clock::duration::max();
    for (std::size_t index = 0; index < threads.size(); ++index) {
      const auto& reading = observed[index];
      if (!reading) continue;
      const auto& thread = threads[index];
      const auto beat = *reading;
      const auto window = std::chrono::duration_cast<Clock::duration>(
                              std::chrono::nanoseconds(thread.period_ns) * 2) +
                          kThreadBeatSlack;
      any_beat = true;
      nonzero_beat = nonzero_beat || beat != 0;
      shortest_window = std::min(shortest_window, window);
      BeatState state{beat, read_at, read_at};
      if (const auto found = beats.find(thread.name); found != beats.end()) {
        const auto& previous = found->second;
        if (previous.beat != beat) {
          if (read_at - previous.read_at <= window) running = true;
        } else {
          state.since = previous.since;
          if (read_at - previous.since >= window) stopped = true;
        }
      }
      next.emplace(thread.name, state);
    }
    beats = std::move(next);
    if (!any_beat) {
      last_running = false;
      return false;
    }
    if (!running && !stopped) {
      const auto decided = probe_beats(read_at, shortest_window);
      running = decided.value_or(last_running.value_or(nonzero_beat));
    }
    last_running = running;
    return running;
  }

  // Re-reads the beats recorded in `beats` until one changes (true), until
  // `window` has passed since `read_at` without a change (false), or for at
  // most kThreadBeatMaxWait (nullopt). Called with beat_mutex held and HAL's
  // mutex not held.
  std::optional<bool> probe_beats(Clock::time_point read_at,
                                  Clock::duration window) {
    const auto give_up = Clock::now() + kThreadBeatMaxWait;
    const auto decide_stopped = read_at + window;
    while (true) {
      const auto wake = std::min(
          {Clock::now() + kThreadBeatPollInterval, decide_stopped, give_up});
      std::this_thread::sleep_until(wake);
      const auto now = Clock::now();
      for (auto& [name, state] : beats) {
        const auto beat = read_thread_beat(name);
        if (!beat) continue;
        if (*beat != state.beat) {
          state = BeatState{*beat, now, now};
          return true;
        }
        state.read_at = now;
      }
      if (now >= decide_stopped) return false;
      if (now >= give_up) return std::nullopt;
    }
  }
};

LinuxCncHalComponent::LinuxCncHalComponent(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

LinuxCncHalComponent::~LinuxCncHalComponent() {
  if (impl_) impl_->close();
}

LinuxCncHalComponent::LinuxCncHalComponent(
    LinuxCncHalComponent&& other) noexcept
    : impl_(std::move(other.impl_)) {}

LinuxCncHalComponent& LinuxCncHalComponent::operator=(
    LinuxCncHalComponent&& other) noexcept {
  if (this == &other) return *this;
  if (impl_) impl_->close();
  impl_ = std::move(other.impl_);
  return *this;
}

int LinuxCncHalComponent::id() const noexcept { return impl_ ? impl_->id : 0; }
const std::string& LinuxCncHalComponent::name() const noexcept {
  static const std::string empty;
  return impl_ ? impl_->name : empty;
}
const std::string& LinuxCncHalComponent::prefix() const noexcept {
  static const std::string empty;
  return impl_ ? impl_->prefix : empty;
}
bool LinuxCncHalComponent::ready() const noexcept {
  return impl_ && impl_->ready;
}

bool LinuxCncHalComponent::add_pin(const std::string& suffix,
                                   HalAdapterType type,
                                   HalAdapterPinDirection direction) {
  if (!impl_) return false;
  const hal_pdir_t native = direction == HalAdapterPinDirection::Out  ? HAL_OUT
                            : direction == HalAdapterPinDirection::Io ? HAL_IO
                                                                      : HAL_IN;
  return impl_->add(suffix, type, true, native);
}

bool LinuxCncHalComponent::add_param(const std::string& suffix,
                                     HalAdapterType type,
                                     HalAdapterParamDirection direction) {
  if (!impl_) return false;
  return impl_->add(
      suffix, type, false,
      direction == HalAdapterParamDirection::ReadWrite ? HAL_RW : HAL_RO);
}

void LinuxCncHalComponent::set_ready() {
  if (!impl_ || impl_->id <= 0)
    throw HalAdapterError("component is not initialized", -EINVAL);
  if (const int result = hal_ready(impl_->id); result != 0)
    throw HalAdapterError("hal_ready failed", result);
  impl_->ready = true;
}

void LinuxCncHalComponent::set_unready() {
  if (!impl_ || impl_->id <= 0)
    throw HalAdapterError("component is not initialized", -EINVAL);
  if (const int result = hal_unready(impl_->id); result != 0)
    throw HalAdapterError("hal_unready failed", result);
  impl_->ready = false;
}

std::optional<HalAdapterValue> LinuxCncHalComponent::read(
    const std::string& suffix) const {
  if (!impl_) return std::nullopt;
  const auto found = impl_->items.find(suffix);
  if (found == impl_->items.end() || !found->second.ref) return std::nullopt;
  return read_owned(found->second.type, *found->second.ref);
}

bool LinuxCncHalComponent::write(const std::string& suffix,
                                 HalAdapterValue value) {
  if (!impl_) return false;
  const auto found = impl_->items.find(suffix);
  if (found == impl_->items.end() || !found->second.ref) return false;
  const auto& item = found->second;
  if (item.pin && item.direction == HAL_IN) return false;
  return write_owned(item.type, *item.ref, value);
}

LinuxCncHalAdapter::LinuxCncHalAdapter(std::string component_name,
                                       std::size_t dynamic_item_capacity)
    : impl_(std::make_unique<Impl>()) {
  if (component_name.empty() || component_name.size() > HAL_NAME_LEN)
    throw HalAdapterError("invalid HAL component name", -EINVAL);
  if (dynamic_item_capacity == 0 ||
      dynamic_item_capacity >
          static_cast<std::size_t>(std::numeric_limits<long>::max()) /
              sizeof(hal_refs_u))
    throw HalAdapterError("invalid HAL dynamic item capacity", -EINVAL);
  impl_->component_name = std::move(component_name);
  impl_->component_id = hal_init(impl_->component_name.c_str());
  if (impl_->component_id <= 0)
    throw HalAdapterError("hal_init failed for '" + impl_->component_name + "'",
                          impl_->component_id);
  if (const int result = hal_ready(impl_->component_id); result != 0) {
    hal_exit(impl_->component_id);
    impl_->component_id = 0;
    throw HalAdapterError(
        "hal_ready failed for '" + impl_->component_name + "'", result);
  }
  try {
    impl_->slab = std::make_shared<HalAllocationSlab>(dynamic_item_capacity);
  } catch (...) {
    hal_exit(impl_->component_id);
    impl_->component_id = 0;
    throw;
  }
}

LinuxCncHalAdapter::~LinuxCncHalAdapter() {
  if (impl_ && impl_->component_id > 0) hal_exit(impl_->component_id);
}

LinuxCncHalAdapter::LinuxCncHalAdapter(LinuxCncHalAdapter&& other) noexcept
    : impl_(std::move(other.impl_)) {}

LinuxCncHalAdapter& LinuxCncHalAdapter::operator=(
    LinuxCncHalAdapter&& other) noexcept {
  if (this == &other) return *this;
  if (impl_ && impl_->component_id > 0) hal_exit(impl_->component_id);
  impl_ = std::move(other.impl_);
  return *this;
}

int LinuxCncHalAdapter::component_id() const noexcept {
  return impl_ ? impl_->component_id : 0;
}

HalAdapterTopology LinuxCncHalAdapter::topology() const {
  HalAdapterTopology result;
  std::vector<std::optional<rtapi_sint>> thread_beats;
  std::chrono::steady_clock::time_point beats_read_at;
  run_locked([&] {
    checked(hal_query::list_components([&](hal_query_t& query) {
              HalAdapterComponentInfo info;
              info.id = query.comp.comp_id;
              info.name = query.name;
              info.kind = component_kind(query.comp.type);
              info.ready = query.comp.ready;
              if (query.comp.pid > 0) info.pid = query.comp.pid;
              result.components.push_back(std::move(info));
            }),
            "hal_list_comp");

    checked(hal_query::list_functions([&](hal_query_t& query) {
              HalAdapterFunctionInfo info;
              info.name = query.name;
              info.owner_id = query.funct.comp_id;
              if (query.funct.comp) info.owner_name = query.funct.comp;
              // HAL no longer tracks a per-function floating-point flag; all
              // functions may use floating point.
              info.uses_fp = true;
              info.reentrant = query.funct.reentrant;
              info.users = query.funct.users;
              result.functions.push_back(std::move(info));
            }),
            "hal_list_funct");
    for (auto& function : result.functions) {
      // HAL publishes the execution statistics as items owned by the
      // function's component: <funct>.time (pin), <funct>.tmax and
      // <funct>.tmax-increased (parameters).
      function.runtime =
          statistic_value(function.name + ".time", HAL_QTYPE_PIN);
      function.max_runtime =
          statistic_value(function.name + ".tmax", HAL_QTYPE_PARAM).value_or(0);
      function.max_runtime_increased =
          statistic_flag(function.name + ".tmax-increased", HAL_QTYPE_PARAM)
              .value_or(false);
    }

    checked(hal_query::list_threads(
                true,
                [&](hal_query_t& query) {
                  if (query.qtype == HAL_QTYPE_THREAD) {
                    HalAdapterThreadInfo info;
                    info.name = query.name;
                    info.period_ns = query.thread.period;
                    info.priority = query.thread.priority;
                    info.uses_fp = true;
                    result.threads.push_back(std::move(info));
                  } else if (query.qtype == HAL_QTYPE_THREAD_FUNCT &&
                             !query.thread.is_init && query.thread.funct &&
                             !result.threads.empty()) {
                    // One-shot init functions are not part of the cyclic
                    // function list reported here.
                    result.threads.back().functions.emplace_back(
                        query.thread.funct);
                  }
                }),
            "hal_list_thread");
    thread_beats.reserve(result.threads.size());
    for (auto& thread : result.threads) {
      thread.runtime = statistic_value(thread.name + ".time", HAL_QTYPE_PIN);
      thread.max_runtime =
          statistic_value(thread.name + ".tmax", HAL_QTYPE_PARAM).value_or(0);
      thread_beats.push_back(read_thread_beat(thread.name));
    }
    beats_read_at = std::chrono::steady_clock::now();

    checked(hal_query::list_pins_params(
                HAL_QTYPE_PIN,
                [&](hal_query_t& query) {
                  const auto converted = adapter_type(query.pp.type);
                  if (!converted) return;
                  HalAdapterPinInfo info;
                  info.name = query.name;
                  info.type = *converted;
                  info.direction = pin_direction(query.pp.dir);
                  info.owner_id = query.pp.comp_id;
                  if (query.pp.signal) info.signal_name = query.pp.signal;
                  info.value = from_hal(query.pp.type, query.pp.value);
                  result.pins.push_back(std::move(info));
                }),
            "hal_list_p");

    checked(hal_query::list_pins_params(
                HAL_QTYPE_PARAM,
                [&](hal_query_t& query) {
                  const auto converted = adapter_type(query.pp.type);
                  if (!converted) return;
                  HalAdapterParamInfo info;
                  info.name = query.name;
                  info.type = *converted;
                  info.direction = param_direction(query.pp.dir);
                  info.owner_id = query.pp.comp_id;
                  info.value = from_hal(query.pp.type, query.pp.value);
                  result.params.push_back(std::move(info));
                }),
            "hal_list_p");

    checked(hal_query::list_signals([&](hal_query_t& query) {
              const auto converted = adapter_type(query.sig.type);
              if (!converted) return;
              HalAdapterSignalInfo info;
              info.name = query.name;
              info.type = *converted;
              info.value = from_hal(query.sig.type, query.sig.value);
              info.readers = query.sig.readers;
              info.writers = query.sig.writers;
              info.bidirs = query.sig.bidirs;
              result.signals.push_back(std::move(info));
            }),
            "hal_list_s");
    for (auto& signal : result.signals) {
      checked(hal_query::list_signal_pins(signal.name.c_str(),
                                          [&](hal_query_t& query) {
                                            if (query.pp.dir != HAL_OUT &&
                                                query.pp.dir != HAL_IO)
                                              return 0;
                                            signal.driver = query.name;
                                            return hal_query::kStop;
                                          }),
              "hal_list_p_s");
    }
  });
  const bool running =
      impl_->threads_running(result.threads, thread_beats, beats_read_at);
  for (auto& thread : result.threads) thread.running = running;
  return result;
}

std::optional<HalAdapterValue> LinuxCncHalAdapter::read(
    const HalAdapterReference& reference) const {
  std::optional<HalAdapterValue> result;
  run_locked([&] { result = read_unlocked(reference); });
  return result;
}

std::vector<std::optional<HalAdapterValue>> LinuxCncHalAdapter::read_many(
    const std::vector<HalAdapterReference>& references,
    const std::stop_token& stop_token) const {
  std::vector<std::optional<HalAdapterValue>> result;
  result.reserve(references.size());
  run_locked([&] {
    for (const auto& reference : references) {
      if (stop_token.stop_requested())
        throw HalAdapterError("HAL read cancelled", -ECANCELED);
      result.push_back(read_unlocked(reference));
    }
  });
  return result;
}

bool LinuxCncHalAdapter::write(const HalAdapterReference& reference,
                               HalAdapterValue value,
                               HalAdapterValue* written) {
  bool applied = false;
  run_locked([&] {
    const auto prepared = prepare_unlocked(reference, value);
    if (!prepared || !apply_unlocked(*prepared)) return;
    if (written) *written = read_back_unlocked(reference);
    applied = true;
  });
  return applied;
}

std::size_t LinuxCncHalAdapter::write_many(
    const std::vector<std::pair<HalAdapterReference, HalAdapterValue>>& updates,
    std::vector<HalAdapterValue>* written, const std::stop_token& stop_token) {
  std::size_t applied = 0;
  run_locked([&] {
    std::vector<PreparedWrite> prepared;
    prepared.reserve(updates.size());
    for (const auto& [reference, value] : updates) {
      if (stop_token.stop_requested())
        throw HalAdapterError("HAL write cancelled", -ECANCELED);
      auto item = prepare_unlocked(reference, value);
      if (!item) return;
      prepared.push_back(*item);
    }
    if (written) {
      written->clear();
      written->reserve(updates.size());
    }
    if (stop_token.stop_requested())
      throw HalAdapterError("HAL write cancelled", -ECANCELED);
    for (const auto& item : prepared) {
      if (!apply_unlocked(item)) return;
      if (written) written->push_back(read_back_unlocked(*item.reference));
      ++applied;
    }
  });
  return applied;
}

bool LinuxCncHalAdapter::create_signal(const std::string& name,
                                       HalAdapterType type) {
  if (name.empty() || name.size() > HAL_NAME_LEN) return false;
  const int result = hal_signal_new(name.c_str(), native_type(type));
  if (result != 0)
    throw HalAdapterError("hal_signal_new failed for '" + name + "'", result);
  return true;
}

bool LinuxCncHalAdapter::pin_has_writer(const std::string& name) const {
  bool found = false;
  bool has_writer = false;
  run_locked([&] {
    const auto pin = find_pp(name, HAL_QTYPE_PIN);
    if (!pin) return;
    found = true;
    if (!pin->pp.signal) return;
    const auto signal = find_signal(pin->pp.signal);
    has_writer = signal && signal->sig.writers > 0;
  });
  if (!found)
    throw HalAdapterError("HAL pin '" + name + "' was not found", -ENOENT);
  return has_writer;
}

bool LinuxCncHalAdapter::component_exists(const std::string& name) const {
  const int result = hal_comp_by_name(name.c_str(), nullptr);
  if (result == -ENOENT) return false;
  checked(result, "hal_comp_by_name");
  return true;
}

bool LinuxCncHalAdapter::component_ready(const std::string& name) const {
  hal_query_t query{};
  const int result = hal_comp_by_name(name.c_str(), &query);
  if (result == -ENOENT) return false;
  checked(result, "hal_comp_by_name");
  return query.comp.ready;
}

int LinuxCncHalAdapter::message_level() const { return rtapi_get_msg_level(); }

void LinuxCncHalAdapter::set_message_level(int level) {
  if (const int result = rtapi_set_msg_level(level); result != 0)
    throw HalAdapterError("rtapi_set_msg_level failed", result);
}

std::unique_ptr<LinuxCncHalComponent> LinuxCncHalAdapter::open_component(
    const std::string& name, const std::string& prefix) {
  if (name.empty() || name.size() > HAL_NAME_LEN)
    throw HalAdapterError("invalid client HAL component name", -EINVAL);
  auto component_impl = std::make_unique<LinuxCncHalComponent::Impl>();
  component_impl->name = name;
  component_impl->prefix = prefix.empty() ? name : prefix;
  component_impl->slab = impl_->slab;
  if (component_impl->prefix.size() > HAL_NAME_LEN)
    throw HalAdapterError("invalid client HAL component prefix", -EINVAL);
  component_impl->id = hal_init(component_impl->name.c_str());
  if (component_impl->id <= 0)
    throw HalAdapterError("hal_init failed for client component '" + name + "'",
                          component_impl->id);
  return std::unique_ptr<LinuxCncHalComponent>(
      new LinuxCncHalComponent(std::move(component_impl)));
}

}  // namespace linuxcnc::server

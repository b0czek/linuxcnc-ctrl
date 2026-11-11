#ifndef ULAPI
#define ULAPI
#endif

#include "linuxcnc_grpc/scope/controller.hpp"

#include <hal.h>
#include <rtapi.h>
#include <rtapi_mutex.h>

#include "config.h"
#include "scope/delta_count.hpp"
#include "scope/shm_abi.h"

// Scope source lookup and thread attachment use the same private HAL lists as
// halcmd. This is the native-only implementation seam; no transport
// abstraction layer crosses it.
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "hal_priv.h"

extern char** environ;

namespace linuxcnc::server {
namespace {

class HalMutex final {
 public:
  HalMutex() {
    if (!hal_data)
      throw ScopeControllerError("HAL is not initialized", -ENODEV);
    rtapi_mutex_get(&hal_data->mutex);
  }
  ~HalMutex() { rtapi_mutex_give(&hal_data->mutex); }
  HalMutex(const HalMutex&) = delete;
  HalMutex& operator=(const HalMutex&) = delete;
};

ScopeState scope_state(linuxcnc_scope_state_t state) {
  switch (state) {
    case LINUXCNC_SCOPE_IDLE:
      return ScopeState::Idle;
    case LINUXCNC_SCOPE_INIT:
      return ScopeState::Init;
    case LINUXCNC_SCOPE_PRE_TRIG:
      return ScopeState::PreTrigger;
    case LINUXCNC_SCOPE_TRIG_WAIT:
      return ScopeState::TriggerWait;
    case LINUXCNC_SCOPE_POST_TRIG:
      return ScopeState::PostTrigger;
    case LINUXCNC_SCOPE_DONE:
      return ScopeState::Done;
    case LINUXCNC_SCOPE_RESET:
      return ScopeState::Reset;
    default:
      return ScopeState::Invalid;
  }
}

bool process_is_live(int pid) {
  if (pid <= 0) return false;
  errno = 0;
  return kill(pid, 0) == 0 || errno == EPERM;
}

bool starts_with(const char* value, std::string_view prefix) {
  return value &&
         std::string_view(value).compare(0, prefix.size(), prefix) == 0;
}

bool has_scope_controller_owner() {
  if (!hal_data) return false;
  HalMutex lock;
  for (SHMFIELD(hal_comp_t) next = hal_data->comp_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* component = static_cast<hal_comp_t*>(SHMPTR(next));
    if (!component) continue;
    if (std::strcmp(component->name, "halscope") == 0) return true;
    if (starts_with(component->name, "linuxcnc-grpc-scope") &&
        process_is_live(component->pid))
      return true;
  }
  return false;
}

void* resolve_scope_source_unlocked(const ScopeSource& source,
                                    hal_type_t* type) {
  if (!type) return nullptr;
  *type = HAL_TYPE_UNSPECIFIED;
  if (source.name.empty()) return nullptr;
  switch (source.kind) {
    case ScopeSourceKind::Param: {
      auto* parameter = halpr_find_param_by_name(source.name.c_str());
      if (!parameter) return nullptr;
      *type = parameter->type;
      return reinterpret_cast<void*>(SHMPTR(parameter->data_ptr));
    }
    case ScopeSourceKind::Pin: {
      auto* pin = halpr_find_pin_by_name(source.name.c_str());
      if (!pin) return nullptr;
      *type = pin->type;
      if (pin->signal) {
        auto* signal = static_cast<hal_sig_t*>(SHMPTR(pin->signal));
        return signal ? reinterpret_cast<void*>(SHMPTR(signal->data_ptr))
                      : nullptr;
      }
      return &pin->dummysig;
    }
    case ScopeSourceKind::Signal: {
      auto* signal = halpr_find_sig_by_name(source.name.c_str());
      if (!signal) return nullptr;
      *type = signal->type;
      return reinterpret_cast<void*>(SHMPTR(signal->data_ptr));
    }
  }
  return nullptr;
}

std::string find_scope_thread_unlocked() {
  auto* scope_function = halpr_find_funct_by_name("scope.sample");
  if (!scope_function) return {};
  for (SHMFIELD(hal_thread_t) next = hal_data->thread_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* thread = static_cast<hal_thread_t*>(SHMPTR(next));
    if (!thread) continue;
    auto* root = &thread->funct_list;
    for (auto* entry = list_next(root); entry != root;
         entry = list_next(entry)) {
      auto* function_entry = reinterpret_cast<hal_funct_entry_t*>(entry);
      auto* function =
          static_cast<hal_funct_t*>(SHMPTR(function_entry->funct_ptr));
      if (function == scope_function) return thread->name;
    }
  }
  return {};
}

bool scope_rt_loaded() {
  HalMutex lock;
  return halpr_find_funct_by_name("scope.sample") != nullptr;
}

int load_scope_rt(std::size_t requested_samples) {
  if (scope_rt_loaded()) return 0;
  const std::string executable = EMC2_BIN_DIR "/halcmd";
  const std::string sample_argument =
      "num_samples=" + std::to_string(requested_samples);
  std::array<char*, 5> arguments{
      const_cast<char*>(executable.c_str()), const_cast<char*>("loadrt"),
      const_cast<char*>("scope_rt"), const_cast<char*>(sample_argument.c_str()),
      nullptr};
  pid_t child = -1;
  const int spawned = posix_spawn(&child, executable.c_str(), nullptr, nullptr,
                                  arguments.data(), environ);
  if (spawned != 0) return -spawned;
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return -errno;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -EIO;
  return scope_rt_loaded() ? 0 : -ENODEV;
}

int source_length(hal_type_t type) {
  switch (type) {
    case HAL_BIT:
      return 1;
    case HAL_FLOAT:
      return sizeof(hal_float_t);
    case HAL_S32:
      return sizeof(hal_s32_t);
    case HAL_U32:
      return sizeof(hal_u32_t);
    default:
      return 0;
  }
}

double sample_value(const linuxcnc_scope_data_t& value, hal_type_t type) {
  switch (type) {
    case HAL_BIT:
      return value.d_u8 ? 1.0 : 0.0;
    case HAL_FLOAT:
      return value.d_real;
    case HAL_S32:
      return value.d_s32;
    case HAL_U32:
      return value.d_u32;
    default:
      return 0.0;
  }
}

std::int64_t sample_period_ns_unlocked(
    const linuxcnc_scope_shm_control_t& control) {
  if (!control.thread_name[0] || !hal_data) return 0;
  HalMutex lock;
  auto* thread = halpr_find_thread_by_name(control.thread_name);
  if (!thread || control.mult <= 0) return 0;
  return static_cast<std::int64_t>(thread->period) * control.mult;
}

void set_trigger_level(linuxcnc_scope_shm_control_t* control, hal_type_t type,
                       double value) {
  switch (type) {
    case HAL_BIT:
      control->trig_level.d_u8 = value != 0.0;
      break;
    case HAL_S32:
      control->trig_level.d_s32 = static_cast<rtapi_s32>(value);
      break;
    case HAL_U32:
      control->trig_level.d_u32 = static_cast<rtapi_u32>(value);
      break;
    default:
      control->trig_level.d_real = value;
      break;
  }
}

void validate_trigger_level(hal_type_t type, double value) {
  if (type == HAL_S32 &&
      (!std::isfinite(value) ||
       value < static_cast<double>(std::numeric_limits<rtapi_s32>::lowest()) ||
       value > static_cast<double>(std::numeric_limits<rtapi_s32>::max()))) {
    throw ScopeControllerError("S32 trigger level is out of range", -ERANGE);
  }
  if (type == HAL_U32 &&
      (!std::isfinite(value) || value < 0.0 ||
       value > static_cast<double>(std::numeric_limits<rtapi_u32>::max()))) {
    throw ScopeControllerError("U32 trigger level is out of range", -ERANGE);
  }
}

}  // namespace

ScopeControllerError::ScopeControllerError(
    // NOLINTNEXTLINE(performance-unnecessary-value-param): public ABI contract
    std::string message, int code)
    : std::runtime_error(message), code_(code) {}

bool ScopeFrameQueue::acquire(const std::string& owner) {
  if (owner.empty()) return false;
  std::lock_guard lock(mutex_);
  if (!owner_.empty() && owner_ != owner) return false;
  owner_ = owner;
  return true;
}

void ScopeFrameQueue::clear(const std::string& owner) {
  std::lock_guard lock(mutex_);
  if (owner_ != owner) return;
  in_flight_.reset();
  pending_.reset();
  delivered_ = false;
  pending_skipped_frames_ = 0;
  condition_.notify_all();
}

void ScopeFrameQueue::release(const std::string& owner) {
  std::lock_guard lock(mutex_);
  if (owner_ != owner) return;
  owner_.clear();
  in_flight_.reset();
  pending_.reset();
  delivered_ = false;
  pending_skipped_frames_ = 0;
  skipped_frames_ = 0;
  condition_.notify_all();
}

bool ScopeFrameQueue::acquired() const {
  std::lock_guard lock(mutex_);
  return !owner_.empty();
}

bool ScopeFrameQueue::publish(ScopeFrame frame) {
  std::function<void()> listener;
  {
    std::lock_guard lock(mutex_);
    if (owner_.empty()) return false;
    frame.generation = next_generation_++;
    frame.skipped_frames = 0;
    if (in_flight_) {
      if (pending_) ++pending_skipped_frames_;
      pending_ = std::move(frame);
    } else {
      delivered_ = false;
      in_flight_ = std::move(frame);
      condition_.notify_all();
    }
    listener = listener_;
  }
  if (listener) listener();
  return true;
}

void ScopeFrameQueue::set_listener(std::function<void()> listener) {
  std::lock_guard lock(mutex_);
  listener_ = std::move(listener);
}

std::optional<ScopeFrame> ScopeFrameQueue::next(const std::string& owner) {
  std::lock_guard lock(mutex_);
  if (owner_ != owner || !in_flight_ || delivered_) return std::nullopt;
  delivered_ = true;
  return in_flight_;
}

std::optional<ScopeFrame> ScopeFrameQueue::wait_next(
    const std::string& owner, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  condition_.wait_for(lock, timeout, [this, &owner] {
    return owner_ != owner || (in_flight_ && !delivered_);
  });
  if (owner_ != owner || !in_flight_ || delivered_) return std::nullopt;
  delivered_ = true;
  return in_flight_;
}

std::optional<ScopeFrame> ScopeFrameQueue::acknowledge(
    const std::string& owner, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (owner_ != owner || !in_flight_ || in_flight_->generation != generation) {
    return std::nullopt;
  }
  in_flight_.reset();
  delivered_ = false;
  if (!pending_) return std::nullopt;
  pending_->skipped_frames = pending_skipped_frames_;
  skipped_frames_ += pending_skipped_frames_;
  pending_skipped_frames_ = 0;
  in_flight_ = std::move(*pending_);
  pending_.reset();
  delivered_ = true;
  condition_.notify_all();
  return in_flight_;
}

std::uint64_t ScopeFrameQueue::skipped_frames() const {
  std::lock_guard lock(mutex_);
  return skipped_frames_;
}

struct LinuxCncScopeControllerState {
  int component_id = 0;
  int shared_memory_id = -1;
  linuxcnc_scope_shm_control_t* control = nullptr;
  linuxcnc_scope_data_t* buffer = nullptr;
  std::string component_name;
  std::chrono::milliseconds poll_period{kScopePollPeriod};
  std::chrono::milliseconds heartbeat_period{kScopeHeartbeatPeriod};

  mutable std::mutex state_mutex;
  std::condition_variable poll_condition;
  std::atomic<bool> polling{false};
  std::thread poll_thread;
  std::string owner;
  ScopeRunMode run_mode = ScopeRunMode::Single;
  bool running = false;
  bool configured = false;
  ScopeConfig config;
  ScopeFrameQueue frames;

  bool delta_initialized = false;
  int delta_start = 0;
  int delta_samples = 0;
  int delta_sample_len = 0;
  std::uint64_t delta_sequence = 0;
  std::chrono::steady_clock::time_point delta_snapshot_at{};
  std::chrono::steady_clock::time_point heartbeat_at{};
};

namespace {

void ensure_attached(const LinuxCncScopeControllerState& impl) {
  if (!impl.control || !impl.buffer || impl.component_id <= 0) {
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  }
}

void ensure_owner(const LinuxCncScopeControllerState& impl,
                  const std::string& owner) {
  if (owner.empty() || impl.owner != owner) {
    throw ScopeControllerError("Scope controller is not owned by this client",
                               -EPERM);
  }
}

ScopeStatus status_unlocked(const LinuxCncScopeControllerState& impl) {
  ensure_attached(impl);
  const auto& control = *impl.control;
  ScopeStatus status;
  status.state = scope_state(control.state);
  status.buffer_length = control.buf_len;
  status.record_length = control.rec_len;
  status.sample_length = control.sample_len;
  status.samples = control.samples;
  status.start = control.start;
  status.multiplier = control.mult;
  status.watchdog = control.watchdog;
  status.thread_name = control.thread_name;
  status.sample_period_ns = sample_period_ns_unlocked(control);
  return status;
}

ScopeCapture copy_capture_unlocked(const LinuxCncScopeControllerState& impl,
                                   bool live) {
  ensure_attached(impl);
  const auto& control = *impl.control;
  const int samples = control.samples;
  const int start = control.start;
  if (samples <= 0)
    throw ScopeControllerError("Scope capture is empty", -EAGAIN);
  if (control.sample_len != static_cast<int>(LINUXCNC_SCOPE_CHANNELS) ||
      samples > control.rec_len || samples > control.buf_len || start < 0 ||
      start >= control.buf_len || control.rec_len <= 0) {
    throw ScopeControllerError("Invalid scope capture bounds", -EPROTO);
  }
  ScopeCapture capture;
  capture.samples = samples;
  capture.trigger_index = live ? samples - 1 : control.pre_trig;
  capture.sample_period_ns = sample_period_ns_unlocked(control);
  int packed_channel = 0;
  for (int channel = 0; channel < static_cast<int>(LINUXCNC_SCOPE_CHANNELS);
       ++channel) {
    if (!control.data_len[channel]) continue;
    auto& values =
        capture.channels[static_cast<std::size_t>(channel)].emplace();
    values.resize(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
      int cell = start + sample * control.sample_len + packed_channel;
      cell %= control.buf_len;
      values[static_cast<std::size_t>(sample)] =
          sample_value(impl.buffer[cell], control.data_type[channel]);
    }
    ++packed_channel;
  }
  return capture;
}

std::optional<ScopeCaptureDelta> copy_delta_unlocked(
    LinuxCncScopeControllerState& impl,
    std::chrono::steady_clock::time_point now) {
  ensure_attached(impl);
  const auto& control = *impl.control;
  const int samples = control.samples;
  const int start = control.start;
  const int sample_length = control.sample_len;
  const int capacity = control.rec_len;
  const int buffer_length = control.buf_len;
  if (samples <= 0) return std::nullopt;
  if (sample_length != static_cast<int>(LINUXCNC_SCOPE_CHANNELS) ||
      capacity <= 0 || samples > capacity || start < 0 ||
      start >= buffer_length) {
    throw ScopeControllerError("Invalid scope delta bounds", -EPROTO);
  }

  const auto period = sample_period_ns_unlocked(control);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - impl.delta_snapshot_at)
                           .count();
  const bool ambiguous_wrap =
      impl.delta_initialized && period > 0 &&
      elapsed >= static_cast<std::int64_t>(capacity) * period;

  bool reset = !impl.delta_initialized ||
               sample_length != impl.delta_sample_len || ambiguous_wrap;
  int count = samples;
  if (!reset) {
    const auto recorded = scope_new_sample_count(
        ScopeRecordPosition{impl.delta_start, impl.delta_samples},
        ScopeRecordPosition{start, samples}, sample_length, buffer_length);
    if (recorded)
      count = *recorded;
    else
      reset = true;
  }
  if (count > samples || count >= capacity) {
    reset = true;
    count = samples;
  }

  impl.delta_initialized = true;
  impl.delta_start = start;
  impl.delta_samples = samples;
  impl.delta_sample_len = sample_length;
  impl.delta_snapshot_at = now;
  if (count == 0) return std::nullopt;
  impl.delta_sequence =
      reset ? static_cast<std::uint64_t>(samples)
            : impl.delta_sequence + static_cast<std::uint64_t>(count);

  ScopeCaptureDelta delta;
  delta.samples = count;
  delta.capacity = capacity;
  delta.sequence = impl.delta_sequence;
  delta.sample_period_ns = period;
  delta.reset = reset;
  const int first = samples - count;
  int packed_channel = 0;
  for (int channel = 0; channel < static_cast<int>(LINUXCNC_SCOPE_CHANNELS);
       ++channel) {
    if (!control.data_len[channel]) continue;
    auto& values = delta.channels[static_cast<std::size_t>(channel)].emplace();
    values.resize(static_cast<std::size_t>(count));
    for (int sample = 0; sample < count; ++sample) {
      int cell = start + (first + sample) * sample_length + packed_channel;
      cell %= buffer_length;
      values[static_cast<std::size_t>(sample)] =
          sample_value(impl.buffer[cell], control.data_type[channel]);
    }
    ++packed_channel;
  }
  return delta;
}

void poll_once(LinuxCncScopeControllerState& impl) {
  std::lock_guard state_lock(impl.state_mutex);
  if (!impl.polling.load(std::memory_order_relaxed) || impl.owner.empty())
    return;
  ensure_attached(impl);
  const auto now = std::chrono::steady_clock::now();
  if (impl.heartbeat_at.time_since_epoch().count() == 0 ||
      now - impl.heartbeat_at >= impl.heartbeat_period) {
    impl.control->watchdog = std::min(impl.control->watchdog + 1, 10);
    impl.heartbeat_at = now;
  }

  try {
    if (impl.run_mode == ScopeRunMode::Roll && impl.running) {
      if (const auto delta = copy_delta_unlocked(impl, now)) {
        ScopeFrame frame;
        frame.kind = ScopeFrameKind::Roll;
        frame.payload = *delta;
        impl.frames.publish(std::move(frame));
      }
      if (impl.control->state == LINUXCNC_SCOPE_DONE) {
        impl.control->state = LINUXCNC_SCOPE_INIT;
      }
      return;
    }
    if (impl.running &&
        (impl.run_mode == ScopeRunMode::Run ||
         impl.run_mode == ScopeRunMode::Single) &&
        impl.control->state == LINUXCNC_SCOPE_DONE) {
      const auto capture = copy_capture_unlocked(impl, false);
      ScopeFrame frame;
      frame.kind = ScopeFrameKind::Capture;
      frame.payload = capture;
      impl.frames.publish(std::move(frame));
      if (impl.run_mode == ScopeRunMode::Run) {
        impl.control->state = LINUXCNC_SCOPE_INIT;
      } else {
        impl.running = false;
      }
    }
  } catch (const std::exception&) {
    // Reset delta state after a teardown race, malformed shared-memory frame,
    // or allocation failure. The worker stays alive and the next good frame is
    // a replacement rather than an unsafe continuation.
    impl.delta_initialized = false;
  }
}

void poll_loop(LinuxCncScopeControllerState& impl) {
  auto next = std::chrono::steady_clock::now();
  while (impl.polling.load(std::memory_order_relaxed)) {
    next += impl.poll_period;
    poll_once(impl);
    std::unique_lock lock(impl.state_mutex);
    impl.poll_condition.wait_until(lock, next, [&impl] {
      return !impl.polling.load(std::memory_order_relaxed);
    });
  }
}

void teardown(LinuxCncScopeControllerState& impl) noexcept {
  impl.polling.store(false, std::memory_order_relaxed);
  impl.poll_condition.notify_all();
  if (impl.poll_thread.joinable()) impl.poll_thread.join();

  if (impl.control) {
    impl.control->state = LINUXCNC_SCOPE_RESET;
    impl.control->force_trig = 0;
    for (int i = 0; i < 250 && impl.control->state != LINUXCNC_SCOPE_IDLE;
         ++i) {
      usleep(1000);
    }
    if (impl.control->state != LINUXCNC_SCOPE_IDLE) {
      const std::string thread = impl.control->thread_name;
      if (!thread.empty())
        hal_del_funct_from_thread("scope.sample", thread.c_str());
      impl.control->curr = 0;
      impl.control->start = 0;
      impl.control->samples = 0;
      impl.control->force_trig = 0;
      impl.control->state = LINUXCNC_SCOPE_IDLE;
    }
  }
  if (impl.shared_memory_id >= 0 && impl.component_id > 0) {
    rtapi_shmem_delete(impl.shared_memory_id, impl.component_id);
  }
  if (impl.component_id > 0) hal_exit(impl.component_id);
  impl.shared_memory_id = -1;
  impl.component_id = 0;
  impl.control = nullptr;
  impl.buffer = nullptr;
}

}  // namespace

LinuxCncScopeController::LinuxCncScopeController(
    std::string component_name, std::chrono::milliseconds poll_period,
    std::chrono::milliseconds heartbeat_period, std::size_t requested_samples)
    : impl_(std::make_unique<LinuxCncScopeControllerState>()) {
  if (component_name.empty() || component_name.size() > HAL_NAME_LEN) {
    throw ScopeControllerError("invalid scope component name", -EINVAL);
  }
  if (poll_period <= std::chrono::milliseconds::zero() ||
      heartbeat_period <= std::chrono::milliseconds::zero()) {
    throw ScopeControllerError("scope polling periods must be positive",
                               -EINVAL);
  }
  if (requested_samples < 1000 || requested_samples > 1000000) {
    throw ScopeControllerError("scope sample count is outside LinuxCNC bounds",
                               -ERANGE);
  }
  impl_->component_name = std::move(component_name);
  impl_->poll_period = poll_period;
  impl_->heartbeat_period = heartbeat_period;
  if (has_scope_controller_owner()) {
    throw ScopeControllerError("another scope controller is already active",
                               -EBUSY);
  }
  impl_->component_id = hal_init(impl_->component_name.c_str());
  if (impl_->component_id <= 0) {
    throw ScopeControllerError("hal_init failed for scope controller",
                               impl_->component_id);
  }
  if (const int loaded = load_scope_rt(requested_samples); loaded != 0) {
    teardown(*impl_);
    throw ScopeControllerError("unable to load LinuxCNC scope_rt", loaded);
  }
  impl_->shared_memory_id =
      rtapi_shmem_new(LINUXCNC_SCOPE_SHM_KEY, impl_->component_id,
                      sizeof(linuxcnc_scope_shm_control_t));
  if (impl_->shared_memory_id < 0) {
    const int error = impl_->shared_memory_id;
    teardown(*impl_);
    throw ScopeControllerError("scope_rt shared memory is unavailable", error);
  }
  void* base = nullptr;
  const int result = rtapi_shmem_getptr(impl_->shared_memory_id, &base);
  if (result < 0 || !base) {
    teardown(*impl_);
    throw ScopeControllerError("unable to map scope_rt shared memory", result);
  }
  impl_->control = static_cast<linuxcnc_scope_shm_control_t*>(base);
  const auto header = (sizeof(linuxcnc_scope_shm_control_t) + 3UL) & ~3UL;
  if (impl_->control->shm_size < header || impl_->control->buf_len <= 0 ||
      impl_->control->shm_size <
          header + static_cast<unsigned long>(impl_->control->buf_len) *
                       sizeof(linuxcnc_scope_data_t)) {
    teardown(*impl_);
    throw ScopeControllerError(
        "invalid or incompatible scope_rt shared-memory ABI", -EPROTO);
  }
  impl_->buffer = reinterpret_cast<linuxcnc_scope_data_t*>(
      static_cast<char*>(base) + header);
  if (!impl_->control->thread_name[0]) {
    HalMutex lock;
    const auto thread = find_scope_thread_unlocked();
    if (!thread.empty()) {
      std::strncpy(impl_->control->thread_name, thread.c_str(), HAL_NAME_LEN);
      impl_->control->thread_name[HAL_NAME_LEN] = '\0';
    }
  }
  if (const int ready = hal_ready(impl_->component_id); ready != 0) {
    teardown(*impl_);
    throw ScopeControllerError("unable to ready scope controller", ready);
  }
  start_polling();
}

LinuxCncScopeController::~LinuxCncScopeController() {
  if (impl_) teardown(*impl_);
}

bool LinuxCncScopeController::acquire(const std::string& owner) {
  if (!impl_ || owner.empty()) return false;
  std::lock_guard lock(impl_->state_mutex);
  ensure_attached(*impl_);
  if (!impl_->frames.acquire(owner)) return false;
  impl_->owner = owner;
  impl_->heartbeat_at = std::chrono::steady_clock::now();
  return true;
}

void LinuxCncScopeController::release(const std::string& owner) {
  if (!impl_) return;
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->owner != owner) return;
  if (impl_->control) {
    impl_->control->state = LINUXCNC_SCOPE_RESET;
    impl_->control->force_trig = 0;
  }
  impl_->running = false;
  impl_->configured = false;
  impl_->delta_initialized = false;
  impl_->frames.release(owner);
  impl_->owner.clear();
}

bool LinuxCncScopeController::acquired() const {
  return impl_ && impl_->frames.acquired();
}

ScopeStatus LinuxCncScopeController::status() const {
  if (!impl_)
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  std::lock_guard lock(impl_->state_mutex);
  return status_unlocked(*impl_);
}

void LinuxCncScopeController::configure(const std::string& owner,
                                        const ScopeConfig& config) {
  if (!impl_)
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  std::lock_guard state_lock(impl_->state_mutex);
  ensure_attached(*impl_);
  ensure_owner(*impl_, owner);
  if (config.thread_name.empty() || config.thread_name.size() > HAL_NAME_LEN ||
      config.trigger_channel < 0 ||
      config.trigger_channel > static_cast<int>(kScopeChannelCount)) {
    throw ScopeControllerError("invalid scope thread or trigger channel",
                               -EINVAL);
  }

  std::array<int, kScopeChannelCount> offsets{};
  std::array<hal_type_t, kScopeChannelCount> types{};
  std::array<char, kScopeChannelCount> lengths{};
  types.fill(HAL_TYPE_UNSPECIFIED);
  long period = 0;
  std::string attached_thread;
  {
    HalMutex hal_lock;
    auto* thread = halpr_find_thread_by_name(config.thread_name.c_str());
    if (thread) period = thread->period;
    for (std::size_t index = 0; index < kScopeChannelCount; ++index) {
      const auto& channel = config.channels[index];
      if (!channel.enabled) continue;
      hal_type_t type = HAL_TYPE_UNSPECIFIED;
      void* data = resolve_scope_source_unlocked(channel.source, &type);
      const int length = source_length(type);
      if (!data || length == 0) {
        throw ScopeControllerError("invalid or unsupported scope source",
                                   -EINVAL);
      }
      offsets[index] = hal_shmoff(data);
      types[index] = type;
      lengths[index] = static_cast<char>(length);
    }
    attached_thread = find_scope_thread_unlocked();
  }
  if (period <= 0)
    throw ScopeControllerError("scope thread is unavailable", -ENOENT);
  const auto max_multiplier =
      static_cast<int>(std::min<long>(1000, 1000000000L / period));
  if (config.multiplier < 1 || config.multiplier > max_multiplier) {
    throw ScopeControllerError("scope multiplier is outside the thread range",
                               -ERANGE);
  }
  if (config.trigger_channel > 0 &&
      !config.channels[static_cast<std::size_t>(config.trigger_channel - 1)]
           .enabled) {
    throw ScopeControllerError("trigger channel is not enabled", -EINVAL);
  }
  const auto trigger_type =
      config.trigger_channel > 0
          ? types[static_cast<std::size_t>(config.trigger_channel - 1)]
          : HAL_FLOAT;
  validate_trigger_level(trigger_type, config.trigger_level);

  if (impl_->control->state != LINUXCNC_SCOPE_IDLE) {
    impl_->control->state = LINUXCNC_SCOPE_RESET;
    for (int i = 0; i < 250 && impl_->control->state != LINUXCNC_SCOPE_IDLE;
         ++i) {
      usleep(1000);
    }
    if (impl_->control->state != LINUXCNC_SCOPE_IDLE) {
      if (!attached_thread.empty())
        hal_del_funct_from_thread("scope.sample", attached_thread.c_str());
      impl_->control->curr = 0;
      impl_->control->start = 0;
      impl_->control->samples = 0;
      impl_->control->force_trig = 0;
      impl_->control->state = LINUXCNC_SCOPE_IDLE;
      attached_thread.clear();
    }
  }
  if (attached_thread != config.thread_name) {
    if (!attached_thread.empty()) {
      if (const int result = hal_del_funct_from_thread("scope.sample",
                                                       attached_thread.c_str());
          result != 0) {
        throw ScopeControllerError(
            "unable to unlink scope.sample from old thread", result);
      }
    }
    if (const int result = hal_add_funct_to_thread(
            "scope.sample", config.thread_name.c_str(), -1);
        result != 0) {
      throw ScopeControllerError("unable to link scope.sample to scope thread",
                                 result);
    }
  }

  std::strncpy(impl_->control->thread_name, config.thread_name.c_str(),
               HAL_NAME_LEN);
  impl_->control->thread_name[HAL_NAME_LEN] = '\0';
  impl_->control->sample_len = static_cast<int>(kScopeChannelCount);
  impl_->control->rec_len =
      impl_->control->buf_len / static_cast<int>(kScopeChannelCount);
  if (impl_->control->rec_len <= 0) {
    throw ScopeControllerError("scope buffer cannot hold one sample", -EPROTO);
  }
  impl_->control->pre_trig = std::clamp(
      config.pre_trigger, 0, std::max(0, impl_->control->rec_len - 1));
  impl_->control->mult = config.multiplier;
  impl_->control->trig_chan = config.trigger_channel;
  set_trigger_level(impl_->control, trigger_type, config.trigger_level);
  impl_->control->trig_edge = config.rising ? 1 : 0;
  impl_->control->auto_trig = config.automatic ? 1 : 0;
  for (std::size_t index = 0; index < kScopeChannelCount; ++index) {
    impl_->control->data_offset[index] = offsets[index];
    impl_->control->data_type[index] = types[index];
    impl_->control->data_len[index] = lengths[index];
  }
  impl_->config = config;
  impl_->configured = true;
  impl_->running = false;
  impl_->delta_initialized = false;
  impl_->delta_sequence = 0;
  impl_->frames.clear(owner);
}

void LinuxCncScopeController::run(const std::string& owner, ScopeRunMode mode) {
  if (!impl_)
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  std::lock_guard lock(impl_->state_mutex);
  ensure_attached(*impl_);
  ensure_owner(*impl_, owner);
  if (!impl_->configured)
    throw ScopeControllerError("scope is not configured", -EINVAL);
  if (impl_->control->state != LINUXCNC_SCOPE_IDLE &&
      impl_->control->state != LINUXCNC_SCOPE_DONE) {
    throw ScopeControllerError("scope is already acquiring", -EBUSY);
  }
  if (mode == ScopeRunMode::Roll) {
    impl_->control->pre_trig = std::max(0, impl_->control->rec_len - 1);
  } else {
    impl_->control->pre_trig = std::clamp(
        impl_->config.pre_trigger, 0, std::max(0, impl_->control->rec_len - 1));
  }
  impl_->delta_initialized = false;
  impl_->delta_sequence = 0;
  impl_->run_mode = mode;
  impl_->running = true;
  impl_->control->state = LINUXCNC_SCOPE_INIT;
}

void LinuxCncScopeController::stop(const std::string& owner) {
  if (!impl_)
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  std::lock_guard lock(impl_->state_mutex);
  ensure_attached(*impl_);
  ensure_owner(*impl_, owner);
  impl_->running = false;
  impl_->delta_initialized = false;
  impl_->control->state = LINUXCNC_SCOPE_RESET;
  impl_->control->force_trig = 0;
  impl_->frames.clear(owner);
}

void LinuxCncScopeController::trigger(const std::string& owner) {
  if (!impl_)
    throw ScopeControllerError("Scope controller is disposed", -ENODEV);
  std::lock_guard lock(impl_->state_mutex);
  ensure_attached(*impl_);
  ensure_owner(*impl_, owner);
  impl_->control->force_trig = 1;
}

std::optional<ScopeFrame> LinuxCncScopeController::next_frame(
    const std::string& owner) {
  if (!impl_) return std::nullopt;
  return impl_->frames.next(owner);
}

std::optional<ScopeFrame> LinuxCncScopeController::wait_frame(
    const std::string& owner, std::chrono::milliseconds timeout) {
  if (!impl_) return std::nullopt;
  return impl_->frames.wait_next(owner, timeout);
}

std::optional<ScopeFrame> LinuxCncScopeController::acknowledge(
    const std::string& owner, std::uint64_t generation) {
  if (!impl_) return std::nullopt;
  return impl_->frames.acknowledge(owner, generation);
}

void LinuxCncScopeController::discard_frames(const std::string& owner) {
  if (impl_) impl_->frames.clear(owner);
}

std::uint64_t LinuxCncScopeController::skipped_frames() const {
  return impl_ ? impl_->frames.skipped_frames() : 0;
}

void LinuxCncScopeController::set_frame_listener(
    std::function<void()> listener) {
  if (impl_) impl_->frames.set_listener(std::move(listener));
}

void LinuxCncScopeController::start_polling() {
  if (!impl_) return;
  bool expected = false;
  if (!impl_->polling.compare_exchange_strong(expected, true,
                                              std::memory_order_relaxed))
    return;
  impl_->poll_thread = std::thread([impl = impl_.get()] { poll_loop(*impl); });
}

void LinuxCncScopeController::stop_polling() {
  if (!impl_) return;
  impl_->polling.store(false, std::memory_order_relaxed);
  impl_->poll_condition.notify_all();
  if (impl_->poll_thread.joinable()) impl_->poll_thread.join();
}

}  // namespace linuxcnc::server

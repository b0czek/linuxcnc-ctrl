#ifndef ULAPI
#define ULAPI
#endif

#include "linuxcnc_grpc/hal/adapter.hpp"

#include <hal.h>
#include <rtapi.h>
#include <rtapi_mutex.h>

// halpr_find_* and the linked-list roots are intentionally private LinuxCNC
// APIs. This adapter is the one narrow native boundary that needs them to
// reproduce halcmd/_hal topology semantics without adding a Node binding layer.
// daemon.
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "hal_priv.h"

namespace linuxcnc::server {
namespace {

class HalMutex final {
 public:
  HalMutex() {
    if (!hal_data) throw HalAdapterError("HAL is not initialized", -ENODEV);
    rtapi_mutex_get(&hal_data->mutex);
  }
  ~HalMutex() { rtapi_mutex_give(&hal_data->mutex); }
  HalMutex(const HalMutex&) = delete;
  HalMutex& operator=(const HalMutex&) = delete;
};

hal_type_t native_type(HalAdapterType type) {
  switch (type) {
    case HalAdapterType::Bit:
      return HAL_BIT;
    case HalAdapterType::Float:
      return HAL_FLOAT;
    case HalAdapterType::S32:
      return HAL_S32;
    case HalAdapterType::U32:
      return HAL_U32;
    case HalAdapterType::S64:
      return HAL_S64;
    case HalAdapterType::U64:
      return HAL_U64;
  }
  return HAL_TYPE_UNSPECIFIED;
}

std::optional<HalAdapterType> adapter_type(hal_type_t type) {
  switch (type) {
    case HAL_BIT:
      return HalAdapterType::Bit;
    case HAL_FLOAT:
      return HalAdapterType::Float;
    case HAL_S32:
      return HalAdapterType::S32;
    case HAL_U32:
      return HalAdapterType::U32;
    case HAL_S64:
      return HalAdapterType::S64;
    case HAL_U64:
      return HalAdapterType::U64;
    default:
      return std::nullopt;
  }
}

HalAdapterPinDirection pin_direction(hal_pin_dir_t direction) {
  if (direction == HAL_OUT) return HalAdapterPinDirection::Out;
  if (direction == HAL_IO) return HalAdapterPinDirection::Io;
  return HalAdapterPinDirection::In;
}

HalAdapterParamDirection param_direction(hal_param_dir_t direction) {
  return direction == HAL_RW ? HalAdapterParamDirection::ReadWrite
                             : HalAdapterParamDirection::ReadOnly;
}

HalAdapterComponentKind component_kind(component_type_t type) {
  switch (type) {
    case COMPONENT_TYPE_USER:
      return HalAdapterComponentKind::User;
    case COMPONENT_TYPE_REALTIME:
      return HalAdapterComponentKind::Realtime;
    case COMPONENT_TYPE_OTHER:
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

HalAdapterValue read_value(HalAdapterType type, const void* pointer) {
  if (!pointer) throw HalAdapterError("HAL item has no data pointer", -EFAULT);
  switch (type) {
    case HalAdapterType::Bit:
      return *static_cast<const hal_bit_t*>(pointer);
    case HalAdapterType::Float:
      return static_cast<double>(*static_cast<const hal_float_t*>(pointer));
    case HalAdapterType::S32:
      return static_cast<std::int32_t>(*static_cast<const hal_s32_t*>(pointer));
    case HalAdapterType::U32:
      return static_cast<std::uint32_t>(
          *static_cast<const hal_u32_t*>(pointer));
    case HalAdapterType::S64:
      return static_cast<std::int64_t>(*static_cast<const hal_s64_t*>(pointer));
    case HalAdapterType::U64:
      return static_cast<std::uint64_t>(
          *static_cast<const hal_u64_t*>(pointer));
  }
  throw HalAdapterError("Unsupported HAL scalar type", -EINVAL);
}

bool write_value(HalAdapterType type, void* pointer,
                 const HalAdapterValue& value) {
  if (!pointer || !same_type(type, value)) return false;
  switch (type) {
    case HalAdapterType::Bit:
      *static_cast<hal_bit_t*>(pointer) = std::get<bool>(value);
      break;
    case HalAdapterType::Float:
      *static_cast<hal_float_t*>(pointer) = std::get<double>(value);
      break;
    case HalAdapterType::S32:
      *static_cast<hal_s32_t*>(pointer) = std::get<std::int32_t>(value);
      break;
    case HalAdapterType::U32:
      *static_cast<hal_u32_t*>(pointer) = std::get<std::uint32_t>(value);
      break;
    case HalAdapterType::S64:
      *static_cast<hal_s64_t*>(pointer) = std::get<std::int64_t>(value);
      break;
    case HalAdapterType::U64:
      *static_cast<hal_u64_t*>(pointer) = std::get<std::uint64_t>(value);
      break;
  }
  return true;
}

void* pin_data(hal_pin_t* pin) {
  if (!pin) return nullptr;
  if (pin->signal) {
    auto* signal = static_cast<hal_sig_t*>(SHMPTR(pin->signal));
    return signal ? reinterpret_cast<void*>(SHMPTR(signal->data_ptr)) : nullptr;
  }
  return &pin->dummysig;
}

std::optional<std::int32_t> runtime_pin_value(const char* owner_name) {
  if (!owner_name) return std::nullopt;
  const std::string pin_name = std::string(owner_name) + ".time";
  if (pin_name.size() > HAL_NAME_LEN) return std::nullopt;
  auto* pin = halpr_find_pin_by_name(pin_name.c_str());
  if (!pin || pin->type != HAL_S32) return std::nullopt;
  const auto* data = static_cast<const hal_s32_t*>(pin_data(pin));
  if (!data) return std::nullopt;
  return static_cast<std::int32_t>(*data);
}

struct ResolvedItem {
  HalAdapterType type = HalAdapterType::Bit;
  void* data = nullptr;
  hal_pin_t* pin = nullptr;
  hal_param_t* param = nullptr;
  hal_sig_t* signal = nullptr;
};

std::optional<ResolvedItem> resolve_unlocked(
    const HalAdapterReference& reference) {
  ResolvedItem resolved;
  if (reference.name.empty()) return std::nullopt;
  switch (reference.kind) {
    case HalAdapterItemKind::Pin: {
      resolved.pin = halpr_find_pin_by_name(reference.name.c_str());
      if (!resolved.pin) return std::nullopt;
      resolved.data = pin_data(resolved.pin);
      break;
    }
    case HalAdapterItemKind::Param: {
      resolved.param = halpr_find_param_by_name(reference.name.c_str());
      if (!resolved.param) return std::nullopt;
      resolved.data = reinterpret_cast<void*>(SHMPTR(resolved.param->data_ptr));
      break;
    }
    case HalAdapterItemKind::Signal: {
      resolved.signal = halpr_find_sig_by_name(reference.name.c_str());
      if (!resolved.signal) return std::nullopt;
      resolved.data =
          reinterpret_cast<void*>(SHMPTR(resolved.signal->data_ptr));
      break;
    }
  }
  hal_type_t type = resolved.pin     ? resolved.pin->type
                    : resolved.param ? resolved.param->type
                                     : resolved.signal->type;
  const auto converted = adapter_type(type);
  if (!converted) return std::nullopt;
  resolved.type = *converted;
  return resolved;
}

std::string full_name(const std::string& prefix, const std::string& suffix) {
  if (suffix.empty()) return {};
  auto result = prefix.empty() ? suffix : prefix + "." + suffix;
  if (result.size() > HAL_NAME_LEN) return {};
  return result;
}

class HalAllocationSlab final {
 public:
  explicit HalAllocationSlab(std::size_t capacity) : capacity_(capacity) {
    static_assert(sizeof(hal_data_u) >= sizeof(void*));
    const auto bytes = static_cast<long>(capacity * sizeof(hal_data_u));
    slots_ = static_cast<hal_data_u*>(hal_malloc(bytes));
    if (!slots_)
      throw HalAdapterError("HAL dynamic item slab allocation failed", -EBUSY);
    std::memset(slots_, 0, capacity * sizeof(hal_data_u));
    free_.reserve(capacity);
    for (std::size_t index = capacity; index > 0; --index)
      free_.push_back(index - 1);
  }

  std::optional<std::size_t> acquire() {
    std::lock_guard lock(mutex_);
    if (free_.empty()) return std::nullopt;
    const auto index = free_.back();
    free_.pop_back();
    std::memset(slots_ + index, 0, sizeof(hal_data_u));
    return index;
  }

  void release(std::size_t index) {
    std::lock_guard lock(mutex_);
    if (index >= capacity_) return;
    std::memset(slots_ + index, 0, sizeof(hal_data_u));
    free_.push_back(index);
  }

  void* data(std::size_t index) { return slots_ + index; }

 private:
  const std::size_t capacity_;
  hal_data_u* slots_ = nullptr;
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
    hal_pin_dir_t pin_direction = HAL_DIR_UNSPECIFIED;
    hal_param_dir_t param_direction = HAL_RO;
    void* data_location = nullptr;
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
};

struct LinuxCncHalAdapter::Impl {
  int component_id = 0;
  std::string component_name;
  std::shared_ptr<HalAllocationSlab> slab;
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
  if (!impl_ || impl_->id <= 0 || impl_->ready || suffix.empty() ||
      impl_->items.find(suffix) != impl_->items.end())
    return false;
  const auto full = full_name(impl_->prefix, suffix);
  if (full.empty()) return false;
  const auto slot = impl_->slab->acquire();
  if (!slot)
    throw HalAdapterError("global HAL dynamic item quota reached", -EBUSY);
  auto item = Impl::Item{suffix,
                         full,
                         type,
                         true,
                         HAL_DIR_UNSPECIFIED,
                         HAL_RO,
                         impl_->slab->data(*slot),
                         *slot};
  item.pin_direction = direction == HalAdapterPinDirection::Out  ? HAL_OUT
                       : direction == HalAdapterPinDirection::Io ? HAL_IO
                                                                 : HAL_IN;
  auto [it, inserted] = impl_->items.emplace(suffix, std::move(item));
  if (!inserted) {
    impl_->slab->release(*slot);
    return false;
  }
  const int result = hal_pin_new(
      it->second.full_name.c_str(), native_type(type), it->second.pin_direction,
      static_cast<void**>(it->second.data_location), impl_->id);
  if (result != 0) {
    impl_->items.erase(it);
    impl_->slab->release(*slot);
    throw HalAdapterError("hal_pin_new failed for '" + full + "'", result);
  }
  return true;
}

bool LinuxCncHalComponent::add_param(const std::string& suffix,
                                     HalAdapterType type,
                                     HalAdapterParamDirection direction) {
  if (!impl_ || impl_->id <= 0 || impl_->ready || suffix.empty() ||
      impl_->items.find(suffix) != impl_->items.end())
    return false;
  const auto full = full_name(impl_->prefix, suffix);
  if (full.empty()) return false;
  const auto slot = impl_->slab->acquire();
  if (!slot)
    throw HalAdapterError("global HAL dynamic item quota reached", -EBUSY);
  auto item = Impl::Item{
      suffix,
      full,
      type,
      false,
      HAL_DIR_UNSPECIFIED,
      direction == HalAdapterParamDirection::ReadWrite ? HAL_RW : HAL_RO,
      impl_->slab->data(*slot),
      *slot};
  auto [it, inserted] = impl_->items.emplace(suffix, std::move(item));
  if (!inserted) {
    impl_->slab->release(*slot);
    return false;
  }
  const int result = hal_param_new(
      it->second.full_name.c_str(), native_type(type),
      it->second.param_direction, it->second.data_location, impl_->id);
  if (result != 0) {
    impl_->items.erase(it);
    impl_->slab->release(*slot);
    throw HalAdapterError("hal_param_new failed for '" + full + "'", result);
  }
  return true;
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
  if (found == impl_->items.end()) return std::nullopt;
  const auto& item = found->second;
  void* data =
      item.pin ? item.data_location && *static_cast<void**>(item.data_location)
                     ? *static_cast<void**>(item.data_location)
                     : nullptr
               : item.data_location;
  if (!data) return std::nullopt;
  return read_value(item.type, data);
}

bool LinuxCncHalComponent::write(const std::string& suffix,
                                 HalAdapterValue value) {
  if (!impl_) return false;
  const auto found = impl_->items.find(suffix);
  if (found == impl_->items.end() || !same_type(found->second.type, value))
    return false;
  auto& item = found->second;
  if (item.pin && item.pin_direction == HAL_IN) return false;
  void* data =
      item.pin ? item.data_location && *static_cast<void**>(item.data_location)
                     ? *static_cast<void**>(item.data_location)
                     : nullptr
               : item.data_location;
  return write_value(item.type, data, value);
}

LinuxCncHalAdapter::LinuxCncHalAdapter(std::string component_name,
                                       std::size_t dynamic_item_capacity)
    : impl_(std::make_unique<Impl>()) {
  if (component_name.empty() || component_name.size() > HAL_NAME_LEN)
    throw HalAdapterError("invalid HAL component name", -EINVAL);
  if (dynamic_item_capacity == 0 ||
      dynamic_item_capacity >
          static_cast<std::size_t>(std::numeric_limits<long>::max()) /
              sizeof(hal_data_u))
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
  HalMutex lock;
  HalAdapterTopology result;
  for (SHMFIELD(hal_comp_t) next = hal_data->comp_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* component = static_cast<hal_comp_t*>(SHMPTR(next));
    HalAdapterComponentInfo info;
    info.id = component->comp_id;
    info.name = component->name;
    info.kind = component_kind(component->type);
    info.ready = component->ready != 0;
    if (component->pid > 0) info.pid = component->pid;
    result.components.push_back(std::move(info));
  }
  for (SHMFIELD(hal_funct_t) next = hal_data->funct_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* function = static_cast<hal_funct_t*>(SHMPTR(next));
    auto* owner = static_cast<hal_comp_t*>(SHMPTR(function->owner_ptr));
    HalAdapterFunctionInfo info;
    info.name = function->name;
    if (owner) {
      info.owner_id = owner->comp_id;
      info.owner_name = owner->name;
    }
    info.uses_fp = function->uses_fp != 0;
    info.reentrant = function->reentrant != 0;
    info.users = function->users;
    info.max_runtime = function->maxtime;
    info.max_runtime_increased = function->maxtime_increased != 0;
    // hal_funct_t::runtime is a process-local pointer written by the
    // realtime component. Resolve the exported <function>.time HAL pin
    // instead of dereferencing that pointer in this daemon process.
    info.runtime = runtime_pin_value(function->name);
    result.functions.push_back(std::move(info));
  }
  for (SHMFIELD(hal_thread_t) next = hal_data->thread_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* thread = static_cast<hal_thread_t*>(SHMPTR(next));
    HalAdapterThreadInfo info;
    info.name = thread->name;
    info.period_ns = thread->period;
    info.priority = thread->priority;
    info.uses_fp = thread->uses_fp != 0;
    info.running = hal_data->threads_running != 0;
    info.max_runtime = thread->maxtime;
    // hal_thread_t::runtime has the same process-local pointer semantics as
    // hal_funct_t::runtime. The shared <thread>.time pin is the safe view.
    info.runtime = runtime_pin_value(thread->name);
    auto* root = &thread->funct_list;
    for (auto* entry = list_next(root); entry != root;
         entry = list_next(entry)) {
      auto* function_entry = reinterpret_cast<hal_funct_entry_t*>(entry);
      auto* function =
          static_cast<hal_funct_t*>(SHMPTR(function_entry->funct_ptr));
      if (function) info.functions.emplace_back(function->name);
    }
    result.threads.push_back(std::move(info));
  }
  for (SHMFIELD(hal_pin_t) next = hal_data->pin_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* pin = static_cast<hal_pin_t*>(SHMPTR(next));
    HalAdapterPinInfo info;
    info.name = pin->name;
    const auto converted = adapter_type(pin->type);
    // HAL_PORT is an implementation-specific internal type; it is not part
    // of the six scalar values exposed by the gRPC HAL oneof.
    if (!converted) continue;
    info.type = *converted;
    info.direction = pin_direction(pin->dir);
    auto* owner = static_cast<hal_comp_t*>(SHMPTR(pin->owner_ptr));
    if (owner) info.owner_id = owner->comp_id;
    if (pin->signal) {
      auto* signal = static_cast<hal_sig_t*>(SHMPTR(pin->signal));
      if (signal) info.signal_name = signal->name;
    }
    info.value = read_value(info.type, pin_data(pin));
    result.pins.push_back(std::move(info));
  }
  for (SHMFIELD(hal_param_t) next = hal_data->param_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* parameter = static_cast<hal_param_t*>(SHMPTR(next));
    HalAdapterParamInfo info;
    info.name = parameter->name;
    const auto converted = adapter_type(parameter->type);
    if (!converted) continue;
    info.type = *converted;
    info.direction = param_direction(parameter->dir);
    auto* owner = static_cast<hal_comp_t*>(SHMPTR(parameter->owner_ptr));
    if (owner) info.owner_id = owner->comp_id;
    info.value = read_value(
        info.type, reinterpret_cast<const void*>(SHMPTR(parameter->data_ptr)));
    result.params.push_back(std::move(info));
  }
  for (SHMFIELD(hal_sig_t) next = hal_data->sig_list_ptr; next;
       next = SHMPTR(next)->next_ptr) {
    auto* signal = static_cast<hal_sig_t*>(SHMPTR(next));
    HalAdapterSignalInfo info;
    info.name = signal->name;
    const auto converted = adapter_type(signal->type);
    if (!converted) continue;
    info.type = *converted;
    info.value = read_value(
        info.type, reinterpret_cast<const void*>(SHMPTR(signal->data_ptr)));
    info.readers = signal->readers;
    info.writers = signal->writers;
    info.bidirs = signal->bidirs;
    for (auto* pin = halpr_find_pin_by_sig(signal, nullptr); pin;
         pin = halpr_find_pin_by_sig(signal, pin)) {
      if (pin->dir == HAL_OUT || pin->dir == HAL_IO) {
        info.driver = pin->name;
        break;
      }
    }
    result.signals.push_back(std::move(info));
  }
  return result;
}

std::optional<HalAdapterValue> LinuxCncHalAdapter::read(
    const HalAdapterReference& reference) const {
  HalMutex lock;
  const auto item = resolve_unlocked(reference);
  if (!item) return std::nullopt;
  return read_value(item->type, item->data);
}

std::vector<std::optional<HalAdapterValue>> LinuxCncHalAdapter::read_many(
    const std::vector<HalAdapterReference>& references,
    const std::stop_token& stop_token) const {
  HalMutex lock;
  std::vector<std::optional<HalAdapterValue>> result;
  result.reserve(references.size());
  for (const auto& reference : references) {
    if (stop_token.stop_requested())
      throw HalAdapterError("HAL read cancelled", -ECANCELED);
    const auto item = resolve_unlocked(reference);
    result.push_back(item ? std::optional<HalAdapterValue>(
                                read_value(item->type, item->data))
                          : std::nullopt);
  }
  return result;
}

bool LinuxCncHalAdapter::write(const HalAdapterReference& reference,
                               HalAdapterValue value,
                               HalAdapterValue* written) {
  HalMutex lock;
  const auto item = resolve_unlocked(reference);
  if (!item || !same_type(item->type, value)) return false;
  if (item->param && item->param->dir != HAL_RW) return false;
  if (item->pin && (item->pin->dir == HAL_OUT || item->pin->signal))
    return false;
  if (item->signal && item->signal->writers > 0) return false;
  if (!write_value(item->type, item->data, value)) return false;
  if (written) *written = read_value(item->type, item->data);
  return true;
}

std::size_t LinuxCncHalAdapter::write_many(
    const std::vector<std::pair<HalAdapterReference, HalAdapterValue>>& updates,
    std::vector<HalAdapterValue>* written, const std::stop_token& stop_token) {
  HalMutex lock;
  std::vector<ResolvedItem> resolved;
  resolved.reserve(updates.size());
  for (const auto& [reference, value] : updates) {
    if (stop_token.stop_requested())
      throw HalAdapterError("HAL write cancelled", -ECANCELED);
    const auto item = resolve_unlocked(reference);
    if (!item || !item->data || !same_type(item->type, value)) return 0;
    if (item->param && item->param->dir != HAL_RW) return 0;
    if (item->pin && (item->pin->dir == HAL_OUT || item->pin->signal)) return 0;
    if (item->signal && item->signal->writers > 0) return 0;
    resolved.push_back(*item);
  }
  if (written) {
    written->clear();
    written->reserve(updates.size());
  }
  if (stop_token.stop_requested())
    throw HalAdapterError("HAL write cancelled", -ECANCELED);
  for (std::size_t index = 0; index < updates.size(); ++index) {
    const auto& value = updates[index].second;
    if (!write_value(resolved[index].type, resolved[index].data, value))
      return index;
    if (written) {
      written->push_back(
          read_value(resolved[index].type, resolved[index].data));
    }
  }
  return updates.size();
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
  HalMutex lock;
  auto* pin = halpr_find_pin_by_name(name.c_str());
  if (!pin)
    throw HalAdapterError("HAL pin '" + name + "' was not found", -ENOENT);
  if (!pin->signal) return false;
  auto* signal = static_cast<hal_sig_t*>(SHMPTR(pin->signal));
  return signal && signal->writers > 0;
}

bool LinuxCncHalAdapter::component_exists(const std::string& name) const {
  HalMutex lock;
  return halpr_find_comp_by_name(name.c_str()) != nullptr;
}

bool LinuxCncHalAdapter::component_ready(const std::string& name) const {
  HalMutex lock;
  auto* component = halpr_find_comp_by_name(name.c_str());
  return component && component->ready != 0;
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

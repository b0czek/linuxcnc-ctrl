#pragma once

#ifndef ULAPI
#define ULAPI
#endif

#include <hal.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "hal/value_codec.hpp"
#include "linuxcnc_grpc/hal/adapter.hpp"

namespace linuxcnc::server {

// The pins and parameters of owned components declared S32 or U32. HAL
// stores them in 64 bits and lists them as S64/U64, but their owner reads them
// clamped to 32 bits (read_owned()). Writes from outside are therefore checked
// against the declared range, so that a value is never acknowledged to the
// writer and then seen differently by the owner. Entries are keyed by the
// name the item was created with (HAL keeps it as the alias's old name when
// the item is renamed by an alias) and matched against the owning component
// id, so a foreign item that reuses a name is never mistaken for an owned one.
class NarrowItemRegistry final {
 public:
  // Returns false, registering nothing, when the name is already registered;
  // HAL then refuses to create the item because the name is taken.
  bool add(bool pin, const std::string& name, int component_id,
           HalAdapterType declared) {
    std::lock_guard lock(mutex_);
    return items_.try_emplace({pin, name}, Entry{component_id, declared})
        .second;
  }

  void remove(bool pin, const std::string& name, int component_id) {
    std::lock_guard lock(mutex_);
    const auto found = items_.find({pin, name});
    if (found != items_.end() && found->second.component_id == component_id)
      items_.erase(found);
  }

  bool empty() const {
    std::lock_guard lock(mutex_);
    return items_.empty();
  }

  // Whether a write of `value` (HAL type `type`) to the item named `name`
  // (with old name `alias` when aliased, else null) reaches its owner
  // unchanged; true for every item that is not an owned narrow item.
  bool accepts(bool pin, const char* name, const char* alias, int component_id,
               hal_type_t type, const hal_query_value_u& value) const {
    std::lock_guard lock(mutex_);
    for (const char* candidate : {name, alias}) {
      if (!candidate) continue;
      const auto found = items_.find({pin, candidate});
      if (found != items_.end() && found->second.component_id == component_id &&
          !hal_codec::fits_declared(found->second.declared, type, value))
        return false;
    }
    return true;
  }

  static bool is_narrow(HalAdapterType type) {
    return type == HalAdapterType::S32 || type == HalAdapterType::U32;
  }

 private:
  struct Entry {
    int component_id = 0;
    HalAdapterType declared = HalAdapterType::S32;
  };
  mutable std::mutex mutex_;
  std::map<std::pair<bool, std::string>, Entry> items_;
};

}  // namespace linuxcnc::server

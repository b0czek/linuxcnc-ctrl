#pragma once

#include <algorithm>
#include <deque>
#include <utility>

#include "linuxcnc/v1/hal.pb.h"

namespace linuxcnc::server::detail {

class ComponentOutbox {
 public:
  struct Entry {
    linuxcnc::v1::HalComponentServerMessage message;
    bool resume_read = false;
  };

  void push_response(linuxcnc::v1::HalComponentServerMessage message) {
    entries_.push_back({std::move(message), true});
  }

  void push_delta(linuxcnc::v1::HalComponentServerMessage message) {
    if (!message.has_delta()) {
      entries_.push_back({std::move(message), false});
      return;
    }
    if (!entries_.empty() && entries_.back().message.has_delta()) {
      merge_delta(message.delta(), entries_.back().message.mutable_delta());
      return;
    }
    entries_.push_back({std::move(message), false});
  }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  Entry pop_front() {
    auto entry = std::move(entries_.front());
    entries_.pop_front();
    return entry;
  }

 private:
  static bool same_item(const linuxcnc::v1::ComponentValue& left,
                        const linuxcnc::v1::ComponentValue& right) {
    return left.item().kind() == right.item().kind() &&
           left.item().name() == right.item().name();
  }

  static void merge_delta(const linuxcnc::v1::HalComponentDelta& source,
                          linuxcnc::v1::HalComponentDelta* target) {
    for (const auto& incoming : source.values()) {
      auto* values = target->mutable_values();
      const auto existing = std::find_if(
          values->begin(), values->end(),
          [&](const auto& value) { return same_item(value, incoming); });
      if (existing == values->end()) {
        *target->add_values() = incoming;
      } else {
        *existing = incoming;
      }
    }
    target->set_sequence(source.sequence());
    target->set_generation(source.generation());
  }

  std::deque<Entry> entries_;
};

}  // namespace linuxcnc::server::detail

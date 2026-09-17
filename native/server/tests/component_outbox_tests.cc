#include <cassert>
#include <cstdint>
#include <string>

#include "hal/grpc/component_outbox.hpp"

namespace {

using linuxcnc::server::detail::ComponentOutbox;
using linuxcnc::v1::HAL_ITEM_KIND_PARAM;
using linuxcnc::v1::HAL_ITEM_KIND_PIN;
using linuxcnc::v1::HAL_TYPE_S32;
using linuxcnc::v1::HalComponentServerMessage;

HalComponentServerMessage delta(
    std::uint64_t sequence, std::string name, std::int32_t value,
    linuxcnc::v1::HalItemKind kind = HAL_ITEM_KIND_PIN) {
  HalComponentServerMessage message;
  message.mutable_delta()->set_generation(7);
  message.mutable_delta()->set_sequence(sequence);
  auto* component_value = message.mutable_delta()->add_values();
  component_value->mutable_item()->set_kind(kind);
  component_value->mutable_item()->set_name(std::move(name));
  component_value->mutable_value()->set_type(HAL_TYPE_S32);
  component_value->mutable_value()->set_s32(value);
  return message;
}

HalComponentServerMessage acknowledgement(std::string, std::int32_t value) {
  HalComponentServerMessage message;
  message.mutable_update_ack()->set_generation(7);
  message.mutable_update_ack()->set_sequence(static_cast<std::uint64_t>(value));
  return message;
}

void adjacent_deltas_are_coalesced_without_losing_items() {
  ComponentOutbox outbox;
  outbox.push_delta(delta(1, "component.first", 10));
  outbox.push_delta(delta(2, "component.second", 20));
  outbox.push_delta(delta(3, "component.first", 30));
  outbox.push_delta(delta(4, "component.first", 40, HAL_ITEM_KIND_PARAM));

  assert(outbox.size() == 1);
  auto entry = outbox.pop_front();
  assert(!entry.resume_read);
  assert(entry.message.delta().sequence() == 4);
  assert(entry.message.delta().values_size() == 3);
  assert(entry.message.delta().values(0).item().name() == "component.first");
  assert(entry.message.delta().values(0).item().kind() == HAL_ITEM_KIND_PIN);
  assert(entry.message.delta().values(0).value().s32() == 30);
  assert(entry.message.delta().values(1).item().name() == "component.second");
  assert(entry.message.delta().values(1).value().s32() == 20);
  assert(entry.message.delta().values(2).item().kind() == HAL_ITEM_KIND_PARAM);
  assert(entry.message.delta().values(2).value().s32() == 40);
}

void acknowledgements_are_never_coalesced_or_reordered() {
  ComponentOutbox outbox;
  outbox.push_delta(delta(1, "component.before", 1));
  outbox.push_response(acknowledgement("component.output", 2));
  outbox.push_delta(delta(2, "component.after", 3));
  outbox.push_delta(delta(3, "component.last", 4));

  assert(outbox.size() == 3);
  auto before = outbox.pop_front();
  auto response = outbox.pop_front();
  auto after = outbox.pop_front();
  assert(before.message.has_delta());
  assert(!before.resume_read);
  assert(response.message.has_update_ack());
  assert(response.resume_read);
  assert(response.message.update_ack().sequence() == 2);
  assert(after.message.has_delta());
  assert(after.message.delta().sequence() == 3);
  assert(after.message.delta().values_size() == 2);
  assert(outbox.empty());
}

}  // namespace

int main() {
  adjacent_deltas_are_coalesced_without_losing_items();
  acknowledgements_are_never_coalesced_or_reordered();
  return 0;
}

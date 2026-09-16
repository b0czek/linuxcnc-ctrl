#include "linuxcnc_grpc/hal/value_telemetry.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>

namespace linuxcnc::server {

struct HalValueTelemetry::State {
  std::string id;
  std::string attachment_token;
  std::chrono::steady_clock::time_point expires;
  std::chrono::steady_clock::time_point next_due;
  std::chrono::milliseconds sample_period{100};
  std::uint64_t revision = 1;
  std::uint64_t sequence = 0;
  std::uint32_t next_slot = 0;
  bool attached = false;
  bool sampled = false;
  std::vector<HalTelemetryBinding> bindings;
  std::vector<std::optional<HalTelemetryValue>> values;
  SubscriptionHub<std::uint64_t> wakes;
};

HalValueTelemetry::HalValueTelemetry(std::size_t capacity,
                                     std::chrono::seconds attachment_ttl)
    : capacity_(capacity), attachment_ttl_(attachment_ttl) {}

std::string HalValueTelemetry::token() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes)
    output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

std::string HalValueTelemetry::key(const HalTelemetryReference& item) {
  return std::to_string(static_cast<unsigned>(item.kind)) + ":" + item.name;
}

HalTelemetryDescriptor HalValueTelemetry::describe(const State& state) const {
  return {state.id, "/v1/hal-values/" + state.attachment_token, state.revision,
          state.sample_period, state.bindings};
}

void HalValueTelemetry::expire_locked(
    std::chrono::steady_clock::time_point now) {
  for (auto iterator = states_.begin(); iterator != states_.end();) {
    const auto& state = iterator->second;
    if (!state->attached && now >= state->expires) {
      tokens_.erase(state->attachment_token);
      state->wakes.close();
      iterator = states_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

bool HalValueTelemetry::within_sampling_budget_locked(
    const std::string* replacing_subscription,
    const std::vector<HalTelemetryResolvedItem>& items,
    std::chrono::milliseconds sample_period) const {
  std::unordered_map<std::string, std::chrono::milliseconds> fastest;
  const auto include = [&fastest](const auto& bindings, const auto period) {
    for (const auto& binding : bindings) {
      const auto item_key = key(binding.item);
      const auto found = fastest.find(item_key);
      if (found == fastest.end() || period < found->second)
        fastest[item_key] = period;
    }
  };
  for (const auto& [id, state] : states_) {
    if (replacing_subscription && id == *replacing_subscription) continue;
    include(state->bindings, state->sample_period);
  }
  include(items, sample_period);
  std::size_t cost = 0;
  for (const auto& [unused, period] : fastest) {
    (void)unused;
    const auto milliseconds = static_cast<std::size_t>(period.count());
    const auto rate = (1000U + milliseconds - 1U) / milliseconds;
    if (rate > kMaxSamplesPerSecond - cost) return false;
    cost += rate;
  }
  return true;
}

std::optional<HalTelemetryDescriptor> HalValueTelemetry::create(
    std::vector<HalTelemetryResolvedItem> items,
    std::chrono::milliseconds sample_period) {
  std::lock_guard lock(mutex_);
  if (closed_) return std::nullopt;
  expire_locked(std::chrono::steady_clock::now());
  if (states_.size() >= capacity_) return std::nullopt;
  if (!within_sampling_budget_locked(nullptr, items, sample_period))
    return std::nullopt;
  auto state = std::make_shared<State>();
  do state->id = token();
  while (states_.count(state->id));
  do state->attachment_token = token();
  while (tokens_.count(state->attachment_token));
  state->sample_period = sample_period;
  state->expires = std::chrono::steady_clock::now() + attachment_ttl_;
  state->next_due = std::chrono::steady_clock::now();
  for (auto& item : items) {
    state->bindings.push_back(
        {state->next_slot++, std::move(item.item), item.type});
    state->values.emplace_back();
  }
  states_.emplace(state->id, state);
  tokens_.emplace(state->attachment_token, state->id);
  return describe(*state);
}

std::optional<HalTelemetryDescriptor> HalValueTelemetry::update(
    const std::string& subscription_id, std::uint64_t expected_revision,
    std::vector<HalTelemetryResolvedItem> items,
    std::chrono::milliseconds sample_period) {
  std::shared_ptr<State> state;
  HalTelemetryDescriptor result;
  std::uint64_t published_sequence = 0;
  {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(subscription_id);
    if (closed_ || found == states_.end()) return std::nullopt;
    state = found->second;
    if (state->revision != expected_revision) return std::nullopt;
    if (!within_sampling_budget_locked(&subscription_id, items, sample_period))
      return std::nullopt;
    std::unordered_map<std::string, std::size_t> previous;
    for (std::size_t index = 0; index < state->bindings.size(); ++index)
      previous.emplace(key(state->bindings[index].item), index);
    std::vector<HalTelemetryBinding> bindings;
    std::vector<std::optional<HalTelemetryValue>> values;
    for (auto& item : items) {
      const auto prior = previous.find(key(item.item));
      if (prior != previous.end() &&
          state->bindings[prior->second].type == item.type) {
        bindings.push_back(state->bindings[prior->second]);
        values.push_back(state->values[prior->second]);
      } else {
        bindings.push_back(
            {state->next_slot++, std::move(item.item), item.type});
        values.emplace_back();
      }
    }
    state->bindings = std::move(bindings);
    state->values = std::move(values);
    state->sample_period = sample_period;
    state->next_due = std::chrono::steady_clock::now();
    state->sampled = state->bindings.empty();
    ++state->revision;
    ++state->sequence;
    published_sequence = state->sequence;
    result = describe(*state);
  }
  state->wakes.publish(published_sequence);
  return result;
}

std::optional<HalTelemetryDescriptor> HalValueTelemetry::descriptor(
    const std::string& subscription_id) const {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(subscription_id);
  return found == states_.end()
             ? std::nullopt
             : std::optional<HalTelemetryDescriptor>(describe(*found->second));
}

std::optional<std::string> HalValueTelemetry::claim(
    const std::string& token_value) {
  std::lock_guard lock(mutex_);
  expire_locked(std::chrono::steady_clock::now());
  const auto token_found = tokens_.find(token_value);
  if (closed_ || token_found == tokens_.end()) return std::nullopt;
  const auto state_found = states_.find(token_found->second);
  if (state_found == states_.end() || state_found->second->attached)
    return std::nullopt;
  auto state = state_found->second;
  state->attached = true;
  state->next_due = std::chrono::steady_clock::now();
  auto id = state->id;
  state->wakes.publish(state->sequence);
  return id;
}

bool HalValueTelemetry::detach(const std::string& subscription_id) {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(subscription_id);
  if (found == states_.end() || !found->second->attached) return false;
  auto& state = *found->second;
  state.attached = false;
  state.expires = std::chrono::steady_clock::now() + attachment_ttl_;
  return true;
}

bool HalValueTelemetry::erase(const std::string& subscription_id) {
  std::shared_ptr<State> state;
  {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(subscription_id);
    if (found == states_.end()) return false;
    state = found->second;
    tokens_.erase(state->attachment_token);
    states_.erase(found);
  }
  state->wakes.publish(state->sequence + 1);
  state->wakes.close();
  return true;
}

void HalValueTelemetry::close() {
  std::vector<std::shared_ptr<State>> states;
  {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    closed_ = true;
    for (auto& item : states_) states.push_back(item.second);
    states_.clear();
    tokens_.clear();
  }
  for (auto& state : states) state->wakes.close();
}

std::vector<HalTelemetryDueSample> HalValueTelemetry::due(
    std::chrono::steady_clock::time_point now) {
  std::vector<HalTelemetryDueSample> result;
  std::lock_guard lock(mutex_);
  expire_locked(now);
  for (auto& [id, state] : states_) {
    if (!state->attached || state->bindings.empty() || now < state->next_due)
      continue;
    state->next_due = now + state->sample_period;
    result.push_back({id, state->revision, state->bindings});
  }
  return result;
}

void HalValueTelemetry::publish(
    const std::string& subscription_id, std::uint64_t revision,
    std::vector<std::optional<HalTelemetryValue>> values) {
  std::shared_ptr<State> state;
  bool changed = false;
  std::uint64_t published_sequence = 0;
  {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(subscription_id);
    if (found == states_.end() || found->second->revision != revision ||
        found->second->values.size() != values.size())
      return;
    state = found->second;
    changed = !state->sampled || state->values != values;
    if (!changed) return;
    state->values = std::move(values);
    state->sampled = true;
    ++state->sequence;
    published_sequence = state->sequence;
  }
  state->wakes.publish(published_sequence);
}

std::optional<HalTelemetrySnapshot> HalValueTelemetry::snapshot(
    const std::string& subscription_id) const {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(subscription_id);
  if (found == states_.end()) return std::nullopt;
  const auto& state = *found->second;
  return HalTelemetrySnapshot{state.sampled, state.revision, state.sequence,
                              state.bindings, state.values};
}

HalValueTelemetry::Subscription HalValueTelemetry::subscribe(
    const std::string& subscription_id,
    SubscriptionHub<std::uint64_t>::Callback callback) {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(subscription_id);
  return found == states_.end()
             ? Subscription{}
             : found->second->wakes.subscribe(std::move(callback));
}

}  // namespace linuxcnc::server

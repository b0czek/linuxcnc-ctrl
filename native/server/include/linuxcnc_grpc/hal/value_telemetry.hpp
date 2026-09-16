#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "linuxcnc_grpc/callback_runtime.hpp"

namespace linuxcnc::server {

enum class HalTelemetryItemKind : std::uint8_t {
  Pin = 1,
  Param = 2,
  Signal = 3
};
enum class HalTelemetryType : std::uint8_t {
  Unavailable = 0,
  Bit = 1,
  Float = 2,
  S32 = 3,
  U32 = 4,
  S64 = 5,
  U64 = 6
};
using HalTelemetryValue =
    std::variant<bool, double, std::int32_t, std::uint32_t, std::int64_t,
                 std::uint64_t>;

struct HalTelemetryReference {
  HalTelemetryItemKind kind = HalTelemetryItemKind::Pin;
  std::string name;
  bool operator==(const HalTelemetryReference& other) const {
    return kind == other.kind && name == other.name;
  }
};

struct HalTelemetryResolvedItem {
  HalTelemetryReference item;
  HalTelemetryType type = HalTelemetryType::Unavailable;
};

struct HalTelemetryBinding {
  std::uint32_t slot = 0;
  HalTelemetryReference item;
  HalTelemetryType type = HalTelemetryType::Unavailable;
};

struct HalTelemetryDescriptor {
  std::string subscription_id;
  std::string websocket_path;
  std::uint64_t revision = 0;
  std::chrono::milliseconds sample_period{100};
  std::vector<HalTelemetryBinding> bindings;
};

struct HalTelemetrySnapshot {
  bool sampled = false;
  std::uint64_t revision = 0;
  std::uint64_t sequence = 0;
  std::vector<HalTelemetryBinding> bindings;
  std::vector<std::optional<HalTelemetryValue>> values;
};

struct HalTelemetryDueSample {
  std::string subscription_id;
  std::uint64_t revision = 0;
  std::vector<HalTelemetryBinding> bindings;
};

class HalValueTelemetry {
 public:
  // 1,024 distinct references at the minimum 50 ms period.
  static constexpr std::size_t kMaxSamplesPerSecond = 20'480;
  using Subscription = SubscriptionHub<std::uint64_t>::Subscription;

  explicit HalValueTelemetry(
      std::size_t capacity = 128,
      std::chrono::seconds attachment_ttl = std::chrono::seconds(30));

  std::optional<HalTelemetryDescriptor> create(
      std::vector<HalTelemetryResolvedItem> items,
      std::chrono::milliseconds sample_period);
  std::optional<HalTelemetryDescriptor> update(
      const std::string& subscription_id, std::uint64_t expected_revision,
      std::vector<HalTelemetryResolvedItem> items,
      std::chrono::milliseconds sample_period);
  std::optional<HalTelemetryDescriptor> descriptor(
      const std::string& subscription_id) const;
  std::optional<std::string> claim(const std::string& token);
  bool detach(const std::string& subscription_id);
  bool erase(const std::string& subscription_id);
  void close();

  std::vector<HalTelemetryDueSample> due(
      std::chrono::steady_clock::time_point now);
  void publish(const std::string& subscription_id, std::uint64_t revision,
               std::vector<std::optional<HalTelemetryValue>> values);
  std::optional<HalTelemetrySnapshot> snapshot(
      const std::string& subscription_id) const;
  Subscription subscribe(const std::string& subscription_id,
                         SubscriptionHub<std::uint64_t>::Callback callback);

 private:
  struct State;
  static std::string token();
  static std::string key(const HalTelemetryReference& item);
  HalTelemetryDescriptor describe(const State& state) const;
  void expire_locked(std::chrono::steady_clock::time_point now);
  bool within_sampling_budget_locked(
      const std::string* replacing_subscription,
      const std::vector<HalTelemetryResolvedItem>& items,
      std::chrono::milliseconds sample_period) const;

  const std::size_t capacity_;
  const std::chrono::seconds attachment_ttl_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<State>> states_;
  std::unordered_map<std::string, std::string> tokens_;
  bool closed_ = false;
};

}  // namespace linuxcnc::server

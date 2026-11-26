#pragma once

#include <cstddef>
#include <cstdint>

#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/position/history.hpp"

namespace linuxcnc::server {

// Shared transport-neutral position state. gRPC owns configuration while
// telemetry transports only observe snapshots and cursor-addressed deltas.
class PositionTelemetry {
 public:
  using Subscription = SubscriptionHub<std::uint64_t>::Subscription;

  explicit PositionTelemetry(std::size_t max_samples = 10000);

  void configure(std::size_t max_samples);
  void clear();
  bool append(const PositionSample& sample);
  PositionHistoryBatch snapshot();
  PositionHistoryBatch since(std::uint64_t sequence, std::uint64_t generation);
  Subscription subscribe(SubscriptionHub<std::uint64_t>::Callback callback);
  void close();

 private:
  PositionHistory history_;
  SubscriptionHub<std::uint64_t> wakes_;
};

}  // namespace linuxcnc::server

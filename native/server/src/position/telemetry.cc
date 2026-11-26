#include "linuxcnc_grpc/position/telemetry.hpp"

namespace linuxcnc::server {

PositionTelemetry::PositionTelemetry(std::size_t max_samples)
    : history_(max_samples) {}

void PositionTelemetry::configure(std::size_t max_samples) {
  history_.configure(max_samples);
  wakes_.publish(history_.next_sequence());
}

void PositionTelemetry::clear() {
  history_.clear();
  wakes_.publish(history_.next_sequence());
}

bool PositionTelemetry::append(const PositionSample& sample) {
  if (!history_.append(sample)) return false;
  wakes_.publish(history_.next_sequence());
  return true;
}

PositionHistoryBatch PositionTelemetry::snapshot() {
  return history_.snapshot();
}

PositionHistoryBatch PositionTelemetry::since(std::uint64_t sequence,
                                              std::uint64_t generation) {
  return history_.since(sequence, 0, generation);
}

PositionTelemetry::Subscription PositionTelemetry::subscribe(
    SubscriptionHub<std::uint64_t>::Callback callback) {
  return wakes_.subscribe(std::move(callback));
}

void PositionTelemetry::close() { wakes_.close(); }

}  // namespace linuxcnc::server

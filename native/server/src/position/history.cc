#include "linuxcnc_grpc/position/history.hpp"

#include <algorithm>
#include <cmath>

namespace linuxcnc::server {

PositionHistory::PositionHistory(std::size_t max_samples, double epsilon)
    : max_samples_(std::max<std::size_t>(1, max_samples)),
      epsilon_(std::max(0.0, epsilon)) {}

void PositionHistory::configure(std::size_t max_samples, double epsilon) {
  std::lock_guard lock(mutex_);
  max_samples_ = std::max<std::size_t>(1, max_samples);
  epsilon_ = std::max(0.0, epsilon);
  while (entries_.size() > max_samples_) entries_.pop_front();
  ++generation_;
}

void PositionHistory::clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  ++generation_;
}

bool PositionHistory::append(const PositionSample& sample) {
  std::lock_guard lock(mutex_);
  if (!entries_.empty() && !changed(entries_.back().sample, sample, epsilon_)) {
    return false;
  }
  // Only a tail no read has returned may absorb a collinear sample; it keeps
  // its sequence, so sequences stay contiguous.
  if (entries_.size() >= 2 && entries_.back().sequence >= sealed_sequence_ &&
      sample.motion_type == entries_.back().sample.motion_type &&
      sample.motion_type == entries_[entries_.size() - 2].sample.motion_type &&
      collinear(sample, entries_.back().sample,
                entries_[entries_.size() - 2].sample)) {
    entries_.back().sample = sample;
  } else {
    entries_.push_back(Entry{next_sequence_++, sample});
  }
  if (entries_.size() > max_samples_) entries_.pop_front();
  return true;
}

PositionHistoryBatch PositionHistory::snapshot() {
  // Cursor zero is either the oldest retained point or behind it, so this
  // always reads the whole retained history.
  auto batch = since(0);
  batch.reset = true;
  return batch;
}

PositionHistoryBatch PositionHistory::since(std::uint64_t sequence,
                                            std::size_t max_samples,
                                            std::uint64_t after_generation) {
  std::lock_guard lock(mutex_);
  PositionHistoryBatch result;
  result.generation = generation_;
  result.next_sequence = next_sequence_;
  result.oldest_sequence =
      entries_.empty() ? next_sequence_ : entries_.front().sequence;
  const bool generation_changed =
      after_generation != 0 && after_generation != generation_;
  // A cursor before the oldest retained point cannot be continued, and one
  // beyond next_sequence_ was not issued by this history.
  result.reset = generation_changed || sequence < result.oldest_sequence ||
                 sequence > next_sequence_;
  const auto first_sequence = result.reset ? result.oldest_sequence : sequence;
  const auto available =
      static_cast<std::size_t>(next_sequence_ - first_sequence);
  const auto count =
      max_samples == 0 ? available : std::min(max_samples, available);
  result.first_sequence = first_sequence;
  result.packed.reserve(count * kPositionStride);
  const auto first =
      entries_.begin() +
      static_cast<std::ptrdiff_t>(first_sequence - result.oldest_sequence);
  for (auto it = first; it != first + static_cast<std::ptrdiff_t>(count);
       ++it) {
    append_packed(it->sample, &result.packed);
  }
  sealed_sequence_ = std::max(sealed_sequence_, first_sequence + count);
  return result;
}

std::size_t PositionHistory::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

std::uint64_t PositionHistory::next_sequence() const {
  std::lock_guard lock(mutex_);
  return next_sequence_;
}

bool PositionHistory::changed(const PositionSample& a, const PositionSample& b,
                              double epsilon) {
  for (std::size_t i = 0; i < a.coordinates.size(); ++i) {
    if (!std::isfinite(a.coordinates[i]) || !std::isfinite(b.coordinates[i])) {
      const bool same_non_finite =
          (std::isnan(a.coordinates[i]) && std::isnan(b.coordinates[i])) ||
          a.coordinates[i] == b.coordinates[i];
      if (!same_non_finite) return true;
      continue;
    }
    if (std::abs(a.coordinates[i] - b.coordinates[i]) > epsilon) return true;
  }
  return a.motion_type != b.motion_type;
}

bool PositionHistory::collinear(const PositionSample& current,
                                const PositionSample& previous,
                                const PositionSample& penultimate) {
  constexpr double kCollinearityEpsilon = 1e-4;
  constexpr double kTiny = 1e-10;
  double current_length_squared = 0.0;
  double previous_length_squared = 0.0;
  double dot = 0.0;
  for (std::size_t index = 0; index < current.coordinates.size(); ++index) {
    const double current_delta =
        current.coordinates[index] - previous.coordinates[index];
    const double previous_delta =
        previous.coordinates[index] - penultimate.coordinates[index];
    if (!std::isfinite(current_delta) || !std::isfinite(previous_delta)) {
      return false;
    }
    current_length_squared += current_delta * current_delta;
    previous_length_squared += previous_delta * previous_delta;
    dot += current_delta * previous_delta;
  }
  const double current_length = std::sqrt(current_length_squared);
  const double previous_length = std::sqrt(previous_length_squared);
  if (current_length < kTiny || previous_length < kTiny) return true;
  return std::abs(1.0 - dot / current_length / previous_length) <
         kCollinearityEpsilon;
}

void PositionHistory::append_packed(const PositionSample& sample,
                                    std::vector<double>* packed) {
  packed->insert(packed->end(), sample.coordinates.begin(),
                 sample.coordinates.end());
  packed->push_back(static_cast<double>(sample.motion_type));
}

}  // namespace linuxcnc::server

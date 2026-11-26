#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace linuxcnc::server {

inline constexpr std::size_t kPositionStride = 10;

struct PositionSample {
  std::array<double, 9> coordinates{};
  std::int32_t motion_type = 0;
};

struct PositionHistoryBatch {
  bool reset = false;
  // A replacement is always emitted by snapshot(). Delta consumers compare
  // this generation with the generation they last acknowledged.
  std::uint64_t generation = 0;
  // Sequence of the first packed point. Point i of packed has sequence
  // first_sequence + i; sequences are contiguous and never reassigned.
  std::uint64_t first_sequence = 0;
  std::uint64_t next_sequence = 0;
  // Sequence of the oldest point the history still retains. Delta consumers
  // drop their points before it instead of receiving a replacement.
  std::uint64_t oldest_sequence = 0;
  std::vector<double> packed;
};

// Bounded, cursor-addressable history used by both the gRPC position service
// and local consumers. It stores the stable ten-double wire layout.
//
// Collinear samples are coalesced into the tail point only while no read has
// returned it. snapshot() and since() seal every point they return, so a
// point a consumer has seen never changes and keeps its sequence.
class PositionHistory {
 public:
  explicit PositionHistory(std::size_t max_samples = 10000,
                           double epsilon = 1e-6);

  void configure(std::size_t max_samples, double epsilon = 1e-6);
  void clear();
  bool append(const PositionSample& sample);
  PositionHistoryBatch snapshot();
  PositionHistoryBatch since(std::uint64_t sequence,
                             std::size_t max_samples = 0,
                             std::uint64_t after_generation = 0);
  std::size_t size() const;
  std::uint64_t next_sequence() const;

 private:
  struct Entry {
    std::uint64_t sequence = 0;
    PositionSample sample;
  };

  static bool changed(const PositionSample& a, const PositionSample& b,
                      double epsilon);
  static bool collinear(const PositionSample& current,
                        const PositionSample& previous,
                        const PositionSample& penultimate);
  static void append_packed(const PositionSample& sample,
                            std::vector<double>* packed);

  mutable std::mutex mutex_;
  std::deque<Entry> entries_;
  std::size_t max_samples_;
  double epsilon_;
  std::uint64_t next_sequence_ = 0;
  // Points before this sequence have been returned by a read and are final.
  std::uint64_t sealed_sequence_ = 0;
  std::uint64_t generation_ = 1;
};

}  // namespace linuxcnc::server

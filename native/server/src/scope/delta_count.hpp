#pragma once

// How many samples scope_rt recorded between two snapshots of its record.

#include <optional>

namespace linuxcnc::server {

// Where scope_rt's record is at one moment: the buffer cell of its oldest
// sample and the number of samples it holds.
struct ScopeRecordPosition {
  int start = 0;
  int samples = 0;
};

// Returns the number of samples recorded between `previous` and `current`,
// or nullopt when the two snapshots do not describe one continuing record.
//
// scope_rt grows `samples` until the record is full and from then on keeps
// `samples` constant and advances `start`, dropping its oldest sample for
// each new one. Both can happen between two snapshots, when the record fills
// in between, so the two movements are added: the record's end, `start` plus
// `samples`, is what moves by one for every new sample.
//
// `sample_length` is the number of buffer cells per sample and
// `buffer_length` the number of cells `start` wraps at; both must be
// positive.
constexpr std::optional<int> scope_new_sample_count(
    ScopeRecordPosition previous, ScopeRecordPosition current,
    int sample_length, int buffer_length) {
  if (current.samples < previous.samples) return std::nullopt;
  const int cells =
      (current.start - previous.start + buffer_length) % buffer_length;
  if (cells % sample_length != 0) return std::nullopt;
  return (current.samples - previous.samples) + cells / sample_length;
}

}  // namespace linuxcnc::server

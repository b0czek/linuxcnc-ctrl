#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "linuxcnc_grpc/linuxcnc/command_completion_tracker.hpp"

using namespace linuxcnc::server;

namespace {

using State = NmlCommandEcho::State;

struct Outcome {
  bool completed = false;
  std::optional<std::string> failure;

  bool pending() const { return !completed && !failure; }
};

void track(NmlCommandCompletionTracker& tracker, std::int32_t serial,
           Outcome& outcome) {
  tracker.track(
      serial, [&outcome] { outcome.completed = true; },
      [&outcome](std::string error) { outcome.failure = std::move(error); });
}

// A tracker that has observed LinuxCNC idle after command 6.
struct IdleTracker {
  IdleTracker() { tracker.observe({6, State::Done}); }
  NmlCommandCompletionTracker tracker{std::chrono::milliseconds::zero()};
};

void stamped_error_rejects_with_its_text_test() {
  IdleTracker idle;
  auto& tracker = idle.tracker;
  Outcome outcome;
  track(tracker, 7, outcome);
  tracker.error_read(7, "Must be in MDI mode to issue MDI command");
  tracker.error_read(7, "second reason");
  tracker.observe({7, State::Error});

  assert(outcome.failure ==
         "Must be in MDI mode to issue MDI command; second reason");
}

void stamped_error_rejects_command_echoed_done_test() {
  // LinuxCNC refuses some commands with an operator error while still
  // reporting the command done, for example a mode switch while a program
  // is running.
  IdleTracker idle;
  auto& tracker = idle.tracker;
  Outcome outcome;
  track(tracker, 7, outcome);
  tracker.error_read(7, "Can't switch mode while mode is AUTO");
  tracker.observe({7, State::Done});

  assert(outcome.failure == "Can't switch mode while mode is AUTO");
}

void back_to_back_rejection_keeps_its_reason_test() {
  // Command 8 was written before any status reported command 7, and the
  // first status read already echoes 8.
  IdleTracker idle;
  auto& tracker = idle.tracker;
  Outcome rejected;
  Outcome next;
  track(tracker, 7, rejected);
  track(tracker, 8, next);
  tracker.error_read(7, "Must be in MDI mode to issue MDI command");
  tracker.observe({8, State::Done});

  assert(rejected.failure == "Must be in MDI mode to issue MDI command");
  assert(next.completed && !next.failure);
}

void unstamped_errors_do_not_resolve_commands_test() {
  IdleTracker idle;
  auto& tracker = idle.tracker;
  Outcome completed;
  track(tracker, 7, completed);
  tracker.error_read(0, "joint 0 following error");
  tracker.observe({7, State::Done});
  assert(completed.completed && !completed.failure);

  Outcome failed;
  track(tracker, 8, failed);
  tracker.error_read(0, "joint 0 following error");
  tracker.observe({8, State::Error});
  assert(failed.failure == NmlCommandCompletionTracker::kRejectedMessage);
}

void stamped_error_read_before_tracking_is_claimed_test() {
  // LinuxCNC can reject a command, and the status and its error can be read,
  // between the command write and track().
  IdleTracker idle;
  auto& tracker = idle.tracker;
  tracker.error_read(7, "Must be in MDI mode to issue MDI command");
  tracker.observe({7, State::Error});

  Outcome outcome;
  track(tracker, 7, outcome);
  assert(outcome.pending());
  tracker.observe({7, State::Error});
  assert(outcome.failure == "Must be in MDI mode to issue MDI command");
}

}  // namespace

int main() {
  stamped_error_rejects_with_its_text_test();
  stamped_error_rejects_command_echoed_done_test();
  back_to_back_rejection_keeps_its_reason_test();
  unstamped_errors_do_not_resolve_commands_test();
  stamped_error_read_before_tracking_is_claimed_test();
  return 0;
}

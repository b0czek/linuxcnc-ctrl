#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "linuxcnc_grpc/command_coordinator.hpp"
#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"

namespace linuxcnc::server::detail {

enum class NmlCompletionRule {
  // A later echoed serial counts as completion. Used for commands task does
  // not queue behind other work.
  EchoAdvance,
  // The result is only meaningful while task still echoes this serial. A later
  // serial means the result was overwritten, so the outcome is unknown.
  ExactEcho,
};

// Resolves written commands from the status channel. Callbacks run on the
// thread that delivers the observation, outside the tracker's lock.
class CommandCompletionTracker {
 public:
  using Completion = std::function<void()>;
  using Failure = std::function<void(std::string)>;

  explicit CommandCompletionTracker(std::chrono::milliseconds timeout)
      : timeout_(timeout) {}

  void track(int serial, NmlCompletionRule rule, Completion completed,
             Failure failed) {
    const auto now = std::chrono::steady_clock::now();
    Pending pending{serial,
                    rule,
                    timeout_ == std::chrono::milliseconds::zero()
                        ? std::chrono::steady_clock::time_point::max()
                        : now + timeout_,
                    0,
                    std::move(completed),
                    std::move(failed)};
    Resolution resolution = Resolution::Pending;
    {
      std::lock_guard lock(mutex_);
      pending.after_observation = observation_sequence_;
      if (!channel_healthy_) {
        resolution = Resolution::Unavailable;
      } else if (have_snapshot_) {
        resolution = resolve(pending, latest_, std::chrono::steady_clock::now(),
                             observation_sequence_);
      }
      if (resolution == Resolution::Pending) {
        pending_.push_back(std::move(pending));
        return;
      }
    }
    finish(pending, resolution);
  }

  void channel_failed() {
    std::vector<Pending> failed;
    {
      std::lock_guard lock(mutex_);
      channel_healthy_ = false;
      failed.swap(pending_);
    }
    for (auto& pending : failed)
      pending.failed("LinuxCNC NML status channel is unavailable");
  }

  void observe(const NmlStatusSnapshot& snapshot) {
    std::vector<std::pair<Pending, Resolution>> ready;
    {
      std::lock_guard lock(mutex_);
      latest_ = snapshot;
      have_snapshot_ = true;
      channel_healthy_ = true;
      observed_at_ = std::chrono::steady_clock::now();
      ++observation_sequence_;
      collect_ready(observed_at_, &ready);
    }
    for (const auto& item : ready) finish(item.first, item.second);
  }

  void tick() {
    std::vector<std::pair<Pending, Resolution>> ready;
    {
      std::lock_guard lock(mutex_);
      collect_ready(std::chrono::steady_clock::now(), &ready);
    }
    for (const auto& item : ready) finish(item.first, item.second);
  }

  bool fresh() const {
    std::lock_guard lock(mutex_);
    return channel_healthy_ && have_snapshot_ &&
           std::chrono::steady_clock::now() - observed_at_ <
               std::chrono::seconds(1);
  }

  // Whether a written command is still waiting for LinuxCNC to finish it.
  bool has_pending() const {
    std::lock_guard lock(mutex_);
    return !pending_.empty();
  }

 private:
  struct Pending {
    int serial;
    NmlCompletionRule rule;
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t after_observation;
    Completion completed;
    Failure failed;
  };

  enum class Resolution {
    Pending,
    Completed,
    Rejected,
    Superseded,
    TimedOut,
    Unavailable
  };

  static Resolution resolve(const Pending& pending,
                            const NmlStatusSnapshot& snapshot,
                            std::chrono::steady_clock::time_point now,
                            std::uint64_t observation_sequence) {
    if (observation_sequence <= pending.after_observation)
      return now >= pending.deadline ? Resolution::TimedOut
                                     : Resolution::Pending;
    if (nml_serial_after(snapshot.echo_serial_number, pending.serial)) {
      return pending.rule == NmlCompletionRule::ExactEcho
                 ? Resolution::Superseded
                 : Resolution::Completed;
    }
    if (snapshot.echo_serial_number == pending.serial) {
      if (snapshot.rcs_status == kNmlRcsStatusDone)
        return Resolution::Completed;
      if (snapshot.rcs_status == kNmlRcsStatusError)
        return Resolution::Rejected;
    }
    return now >= pending.deadline ? Resolution::TimedOut : Resolution::Pending;
  }

  void collect_ready(std::chrono::steady_clock::time_point now,
                     std::vector<std::pair<Pending, Resolution>>* ready) {
    auto pending = pending_.begin();
    while (pending != pending_.end()) {
      const auto resolution =
          resolve(*pending, latest_, now, observation_sequence_);
      if (resolution == Resolution::Pending) {
        ++pending;
        continue;
      }
      ready->emplace_back(std::move(*pending), resolution);
      pending = pending_.erase(pending);
    }
  }

  static void finish(const Pending& pending, Resolution resolution) {
    switch (resolution) {
      case Resolution::Completed:
        pending.completed();
        break;
      case Resolution::Rejected:
        pending.failed("LinuxCNC rejected the command");
        break;
      case Resolution::Superseded:
        pending.failed(
            "LinuxCNC moved on to a later command before reporting this "
            "command's result; its outcome is unknown");
        break;
      case Resolution::TimedOut:
        pending.failed("timed out waiting for LinuxCNC command completion");
        break;
      case Resolution::Unavailable:
        pending.failed("LinuxCNC NML status channel is unavailable");
        break;
      case Resolution::Pending:
        break;
    }
  }

  mutable std::mutex mutex_;
  NmlStatusSnapshot latest_;
  std::vector<Pending> pending_;
  std::chrono::steady_clock::time_point observed_at_{};
  bool have_snapshot_ = false;
  bool channel_healthy_ = false;
  std::uint64_t observation_sequence_ = 0;
  std::chrono::milliseconds timeout_{};
};

// Finishes a command already written to LinuxCNC: defers its ticket to the
// tracker and, for a task-queued command, holds later normal commands until
// the ticket resolves. Must run inside the command's coordinator action.
inline void track_written_command(CommandCompletionTracker* tracker,
                                  CommandContext& context, int serial,
                                  bool task_queued,
                                  std::chrono::milliseconds completion_timeout,
                                  std::function<void()> on_completed,
                                  std::function<void()> on_failed) {
  context.mark_accepted(static_cast<std::uint32_t>(serial));
  context.defer_completion();
  std::function<void()> release_hold = [] {};
  if (task_queued) {
    release_hold = context.hold_normal_commands(
        completion_timeout > std::chrono::milliseconds::zero()
            ? completion_timeout
            : std::chrono::milliseconds(kNmlTaskQueuedHoldLimit));
  }
  tracker->track(
      serial,
      task_queued ? NmlCompletionRule::ExactEcho
                  : NmlCompletionRule::EchoAdvance,
      [completed = context.mark_completed, failed = context.mark_failed,
       on_completed = std::move(on_completed), release_hold]() mutable {
        try {
          if (on_completed) on_completed();
          completed();
        } catch (const std::exception& error) {
          failed(error.what());
        } catch (...) {
          failed("command completion callback failed");
        }
        release_hold();
      },
      [failed = context.mark_failed, on_failed = std::move(on_failed),
       release_hold](std::string error) mutable {
        try {
          if (on_failed) on_failed();
        } catch (const std::exception& rollback_error) {
          error += "; rollback failed: ";
          error += rollback_error.what();
        } catch (...) {
          error += "; rollback failed";
        }
        failed(std::move(error));
        release_hold();
      });
}

}  // namespace linuxcnc::server::detail

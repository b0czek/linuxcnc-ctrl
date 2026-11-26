#include "linuxcnc_grpc/linuxcnc/command_completion_tracker.hpp"

#include <algorithm>
#include <utility>

namespace linuxcnc::server {

NmlCommandCompletionTracker::NmlCommandCompletionTracker(
    std::chrono::milliseconds timeout)
    : timeout_(timeout) {}

void NmlCommandCompletionTracker::track(std::int32_t serial,
                                        NmlCompletionRule rule,
                                        Completion completed, Failure failed) {
  const auto now = std::chrono::steady_clock::now();
  Pending pending{serial,
                  rule,
                  timeout_ == std::chrono::milliseconds::zero()
                      ? std::chrono::steady_clock::time_point::max()
                      : now + timeout_,
                  0,
                  std::move(completed),
                  std::move(failed)};
  {
    std::lock_guard lock(mutex_);
    if (channel_healthy_) {
      // Only a status read after this point can show the command, even when
      // an earlier read already echoes its serial: LinuxCNC may have
      // restarted and reused it.
      pending.after_observation = observation_sequence_;
      pending_.push_back(std::move(pending));
      return;
    }
  }
  finish({std::move(pending), Resolution::Unavailable, {}});
}

void NmlCommandCompletionTracker::channel_failed() {
  std::vector<Pending> failed;
  {
    std::lock_guard lock(mutex_);
    channel_healthy_ = false;
    failed.swap(pending_);
  }
  for (auto& pending : failed)
    pending.failed("LinuxCNC NML status channel is unavailable");
}

void NmlCommandCompletionTracker::error_read(std::int32_t command_serial,
                                             std::string message) {
  if (command_serial == 0) return;
  std::lock_guard lock(mutex_);
  if (command_errors_.size() >= kMaxUnclaimedErrors)
    command_errors_.pop_front();
  command_errors_.push_back(
      {command_serial, std::move(message), std::chrono::steady_clock::now()});
}

void NmlCommandCompletionTracker::observe(const NmlCommandEcho& echo) {
  std::vector<Ready> ready;
  {
    std::lock_guard lock(mutex_);
    latest_ = echo;
    have_snapshot_ = true;
    channel_healthy_ = true;
    observed_at_ = std::chrono::steady_clock::now();
    ++observation_sequence_;
    collect_ready(observed_at_, &ready);
  }
  for (const auto& item : ready) finish(item);
}

void NmlCommandCompletionTracker::tick() {
  std::vector<Ready> ready;
  {
    std::lock_guard lock(mutex_);
    collect_ready(std::chrono::steady_clock::now(), &ready);
  }
  for (const auto& item : ready) finish(item);
}

bool NmlCommandCompletionTracker::has_pending() const {
  std::lock_guard lock(mutex_);
  return !pending_.empty();
}

bool NmlCommandCompletionTracker::fresh() const {
  std::lock_guard lock(mutex_);
  return channel_healthy_ && have_snapshot_ &&
         std::chrono::steady_clock::now() - observed_at_ <
             std::chrono::seconds(1);
}

NmlCommandCompletionTracker::Resolution NmlCommandCompletionTracker::resolve(
    const Pending& pending, std::chrono::steady_clock::time_point now,
    std::string* reason) {
  const bool reached =
      have_snapshot_ && observation_sequence_ > pending.after_observation &&
      (latest_.serial == pending.serial ||
       detail::nml_serial_after(latest_.serial, pending.serial));
  if (reached) {
    bool stamped = false;
    for (const auto& error : command_errors_) {
      if (error.serial != pending.serial) continue;
      stamped = true;
      if (error.message.empty()) continue;
      if (!reason->empty()) *reason += "; ";
      *reason += error.message;
    }
    if (stamped) return Resolution::Rejected;
    if (latest_.serial != pending.serial) {
      return pending.rule == NmlCompletionRule::ExactEcho
                 ? Resolution::Superseded
                 : Resolution::Completed;
    }
    if (latest_.state == NmlCommandEcho::State::Done)
      return Resolution::Completed;
    if (latest_.state == NmlCommandEcho::State::Error)
      return Resolution::Rejected;
  }
  return now >= pending.deadline ? Resolution::TimedOut : Resolution::Pending;
}

void NmlCommandCompletionTracker::collect_ready(
    std::chrono::steady_clock::time_point now, std::vector<Ready>* ready) {
  auto pending = pending_.begin();
  while (pending != pending_.end()) {
    std::string reason;
    const auto resolution = resolve(*pending, now, &reason);
    if (resolution == Resolution::Pending) {
      ++pending;
      continue;
    }
    const auto serial = pending->serial;
    std::erase_if(command_errors_, [serial](const CommandError& error) {
      return error.serial == serial;
    });
    ready->push_back({std::move(*pending), resolution, std::move(reason)});
    pending = pending_.erase(pending);
  }
  forget_unclaimed_errors(now);
}

bool NmlCommandCompletionTracker::is_pending(std::int32_t serial) const {
  return std::any_of(
      pending_.begin(), pending_.end(),
      [serial](const Pending& pending) { return pending.serial == serial; });
}

void NmlCommandCompletionTracker::forget_unclaimed_errors(
    std::chrono::steady_clock::time_point now) {
  std::erase_if(command_errors_, [this, now](const CommandError& error) {
    return now - error.read_at >= kUnclaimedErrorRetention &&
           !is_pending(error.serial);
  });
}

void NmlCommandCompletionTracker::finish(const Ready& ready) {
  switch (ready.resolution) {
    case Resolution::Completed:
      ready.pending.completed();
      break;
    case Resolution::Rejected:
      ready.pending.failed(ready.reason.empty() ? kRejectedMessage
                                                : ready.reason);
      break;
    case Resolution::Superseded:
      ready.pending.failed(
          "LinuxCNC moved on to a later command before reporting this "
          "command's result; its outcome is unknown");
      break;
    case Resolution::TimedOut:
      ready.pending.failed("timed out waiting for LinuxCNC command completion");
      break;
    case Resolution::Unavailable:
      ready.pending.failed("LinuxCNC NML status channel is unavailable");
      break;
    case Resolution::Pending:
      break;
  }
}

}  // namespace linuxcnc::server

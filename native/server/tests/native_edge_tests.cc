#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "linuxcnc_grpc/command_coordinator.hpp"
#include "linuxcnc_grpc/gcode/canon_preview.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"
#include "linuxcnc_grpc/linuxcnc/command_completion_tracker.hpp"
#include "linuxcnc_grpc/linuxcnc/command_validation.hpp"
#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"
#include "linuxcnc_grpc/position/history.hpp"
#include "linuxcnc_grpc/program/workspace.hpp"
#include "linuxcnc_grpc/program/workspace_archive.hpp"

namespace fs = std::filesystem;
using namespace linuxcnc::server;

namespace {

CommandTicket submitted(CommandSubmission submission) {
  assert(submission.status == CommandSubmitStatus::Submitted);
  return std::move(submission.ticket);
}

class Gate {
 public:
  void arrive() {
    {
      std::lock_guard lock(mutex_);
      arrived_ = true;
    }
    condition_.notify_all();
  }

  void wait_for_arrival() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return arrived_; });
  }

  void open() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    condition_.notify_all();
  }

  void wait_until_open() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool arrived_ = false;
  bool open_ = false;
};

void command_queue_bounds_and_wait_test() {
  CommandCoordinator coordinator(2);
  Gate first_action;
  auto first = submitted(coordinator.submit([&] {
    first_action.arrive();
    first_action.wait_until_open();
  }));
  first_action.wait_for_arrival();

  // The first item is in flight, so exactly two more items fit in the queue.
  const auto second = submitted(coordinator.submit([] {}));
  const auto third = submitted(coordinator.submit([] {}));
  assert(coordinator.queued() == 2);
  assert(coordinator.submit([] {}).status == CommandSubmitStatus::QueueFull);
  assert(first.sequence() == 1);
  assert(second.sequence() == 2);
  assert(third.sequence() == 3);

  CommandResult result;
  assert(!first.wait_for(CommandWaitPolicy::Accepted,
                         std::chrono::milliseconds(1), &result));
  assert(result.state == CommandState::Queued);
  first_action.open();
  assert(first.wait_for(CommandWaitPolicy::Accepted, std::chrono::seconds(1),
                        &result));
  assert(result.state == CommandState::Accepted ||
         result.state == CommandState::Completed);
  assert(first.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Completed);
  assert(second.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Completed);
  assert(third.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Completed);
  assert(coordinator.queued() == 0);
  coordinator.shutdown();

  assert(coordinator.submit([] {}).status == CommandSubmitStatus::Stopped);
}

void command_cancellation_and_acceptance_test() {
  CommandCoordinator coordinator(1);
  Gate action;
  std::stop_source stop_source;
  stop_source.request_stop();
  std::atomic<bool> cancellation_seen{false};
  auto ticket = submitted(coordinator.submit_with_context(
      [&](CommandContext& context) {
        cancellation_seen = context.stop_token.stop_requested();
        action.arrive();
        context.mark_accepted(101);
        action.wait_until_open();
      },
      stop_source.get_token()));
  action.wait_for_arrival();

  CommandResult result;
  assert(ticket.wait_for(CommandWaitPolicy::Accepted, std::chrono::seconds(1),
                         &result));
  assert(result.state == CommandState::Accepted);
  assert(result.accepted_sequence == 101);
  assert(cancellation_seen.load());
  assert(!ticket.wait_for(CommandWaitPolicy::Completed,
                          std::chrono::milliseconds(1), &result));
  assert(result.state == CommandState::Accepted);

  // Cancellation belongs to the waiting RPC; it must not remove queued work.
  action.open();
  assert(ticket.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Completed);
  coordinator.shutdown();
}

void command_cancellation_before_worker_start_test() {
  CommandCoordinator coordinator(2);
  Gate blocker;
  auto first = submitted(coordinator.submit([&] {
    blocker.arrive();
    blocker.wait_until_open();
  }));
  blocker.wait_for_arrival();
  std::stop_source stop_source;
  stop_source.request_stop();
  std::atomic<bool> observed{false};
  auto queued = submitted(coordinator.submit_with_context(
      [&](CommandContext& context) {
        observed = context.stop_token.stop_requested();
        context.mark_accepted(202);
      },
      stop_source.get_token()));
  blocker.open();
  CommandResult result;
  assert(first.wait_for(std::chrono::seconds(1), &result));
  assert(queued.wait_for(std::chrono::seconds(1), &result));
  assert(observed.load());
  assert(result.state == CommandState::Completed);
  coordinator.shutdown();
}

void command_failure_wait_test() {
  CommandCoordinator coordinator(1);
  const auto ticket = submitted(coordinator.submit(
      [] { throw std::runtime_error("synthetic failure"); }));
  CommandResult result;
  assert(ticket.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Failed);
  assert(result.error == "synthetic failure");
  assert(ticket.wait().state == CommandState::Failed);
  coordinator.shutdown();
}

void command_priority_and_reserved_capacity_test() {
  CommandCoordinator coordinator(1, 1);
  Gate blocker;
  std::mutex order_mutex;
  std::vector<int> order;
  auto first = submitted(coordinator.submit([&] {
    blocker.arrive();
    blocker.wait_until_open();
    std::lock_guard lock(order_mutex);
    order.push_back(1);
  }));
  blocker.wait_for_arrival();
  auto normal = submitted(coordinator.submit([&] {
    std::lock_guard lock(order_mutex);
    order.push_back(2);
  }));
  auto safety = submitted(coordinator.submit(
      [&] {
        std::lock_guard lock(order_mutex);
        order.push_back(3);
      },
      CommandPriority::Safety));

  assert(coordinator.submit([] {}, CommandPriority::Safety).status ==
         CommandSubmitStatus::QueueFull);
  assert(coordinator.queued() == 2);

  blocker.open();
  CommandResult result;
  assert(first.wait_for(std::chrono::seconds(1), &result));
  assert(safety.wait_for(std::chrono::seconds(1), &result));
  assert(normal.wait_for(std::chrono::seconds(1), &result));
  assert(order == std::vector<int>({1, 3, 2}));
  coordinator.shutdown();
}

void deferred_command_completion_test() {
  CommandCoordinator coordinator(1);
  std::function<void()> complete;
  std::function<void(std::string)> fail;
  auto ticket =
      submitted(coordinator.submit_with_context([&](CommandContext& context) {
        complete = context.mark_completed;
        fail = context.mark_failed;
        context.defer_completion();
        context.mark_accepted(303);
      }));

  CommandResult result;
  assert(ticket.wait_for(CommandWaitPolicy::Accepted, std::chrono::seconds(1),
                         &result));
  assert(result.state == CommandState::Accepted);
  assert(result.accepted_sequence == 303);
  assert(!ticket.wait_for(CommandWaitPolicy::Completed,
                          std::chrono::milliseconds(1), &result));
  complete();
  assert(ticket.wait_for(std::chrono::seconds(1), &result));
  assert(result.state == CommandState::Completed);
  fail("late failure");
  assert(ticket.wait().state == CommandState::Completed);
  coordinator.shutdown();
}

// Unlike assert(), stays active when NDEBUG is defined.
void require(bool condition, const char* description) {
  if (condition) return;
  std::cerr << "command hold check failed: " << description << "\n";
  std::abort();
}

constexpr auto kSettle = std::chrono::milliseconds(40);
constexpr auto kPatience = std::chrono::seconds(5);

// The order in which commands reached the simulated LinuxCNC channel.
class WriteLog {
 public:
  void add(int serial) {
    {
      std::lock_guard lock(mutex_);
      writes_.push_back(serial);
    }
    condition_.notify_all();
  }

  bool wait_for(int serial) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, kPatience, [&] {
      return std::find(writes_.begin(), writes_.end(), serial) != writes_.end();
    });
  }

  std::vector<int> writes() const {
    std::lock_guard lock(mutex_);
    return writes_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<int> writes_;
};

CommandResult finished(const CommandTicket& ticket, const char* description) {
  CommandResult result;
  require(ticket.wait_for(kPatience, &result), description);
  return result;
}

// Starts a hold from inside a command and returns its release callable.
std::function<void()> start_hold(CommandCoordinator& coordinator,
                                 std::chrono::milliseconds limit) {
  std::function<void()> release;
  const auto ticket =
      submitted(coordinator.submit_with_context([&](CommandContext& context) {
        release = context.hold_normal_commands(limit);
      }));
  (void)finished(ticket, "the holding command finishes");
  return release;
}

CommandTicket logged(CommandCoordinator& coordinator, WriteLog& log, int serial,
                     CommandPriority priority = CommandPriority::Normal,
                     CommandHoldPolicy policy = CommandHoldPolicy::Wait) {
  return submitted(coordinator.submit_with_context(
      [&log, serial](CommandContext&) { log.add(serial); }, {}, priority,
      policy));
}

void command_hold_keeps_later_commands_in_order_test() {
  CommandCoordinator coordinator(4);
  WriteLog log;
  const auto release = start_hold(coordinator, std::chrono::seconds(30));
  const auto first = logged(coordinator, log, 1);
  const auto second = logged(coordinator, log, 2);
  std::this_thread::sleep_for(kSettle);
  require(log.writes().empty(), "held commands do not start");
  require(coordinator.queued() == 2, "held commands stay queued");
  release();
  (void)finished(first, "first held command runs after release");
  (void)finished(second, "second held command runs after release");
  require(log.writes() == std::vector<int>({1, 2}),
          "held commands keep their order");
  release();  // A repeated release is harmless.
  coordinator.shutdown();
}

void command_hold_bypass_test() {
  CommandCoordinator coordinator(4, 2);
  WriteLog log;
  const auto release = start_hold(coordinator, std::chrono::seconds(30));
  const auto held = logged(coordinator, log, 1);
  const auto safety = logged(coordinator, log, 2, CommandPriority::Safety);
  (void)finished(safety, "a safety command passes a hold");
  const auto bypass = logged(coordinator, log, 3, CommandPriority::Normal,
                             CommandHoldPolicy::Bypass);
  (void)finished(bypass, "a bypassing command passes a hold");
  std::this_thread::sleep_for(kSettle);
  require(log.writes() == std::vector<int>({2, 3}),
          "ordinary commands stay held while others pass");
  release();
  (void)finished(held, "the held command runs after release");
  require(log.writes() == std::vector<int>({2, 3, 1}),
          "the held command runs last");
  coordinator.shutdown();
}

void command_hold_bypass_keeps_order_without_hold_test() {
  CommandCoordinator coordinator(4);
  WriteLog log;
  Gate blocker;
  const auto busy = submitted(coordinator.submit([&] {
    blocker.arrive();
    blocker.wait_until_open();
  }));
  blocker.wait_for_arrival();
  const auto first = logged(coordinator, log, 1);
  const auto second = logged(coordinator, log, 2, CommandPriority::Normal,
                             CommandHoldPolicy::Bypass);
  blocker.open();
  (void)finished(busy, "the blocking command finishes");
  (void)finished(first, "the first command runs");
  (void)finished(second, "the second command runs");
  require(log.writes() == std::vector<int>({1, 2}),
          "bypass does not reorder commands when nothing is held");
  coordinator.shutdown();
}

void command_hold_expiry_test() {
  CommandCoordinator coordinator(4);
  WriteLog log;
  const auto started = std::chrono::steady_clock::now();
  const auto expired = start_hold(coordinator, kSettle);
  const auto after_expiry = logged(coordinator, log, 1);
  (void)finished(after_expiry, "a hold ends at its limit without a release");
  require(std::chrono::steady_clock::now() - started >= kSettle,
          "a hold lasts until its limit");

  // A late release of the expired hold must not end a newer hold.
  const auto release = start_hold(coordinator, std::chrono::seconds(30));
  expired();
  const auto held = logged(coordinator, log, 2);
  std::this_thread::sleep_for(kSettle);
  require(log.writes() == std::vector<int>({1}),
          "a stale release leaves the newer hold in place");
  release();
  (void)finished(held, "the current release ends the hold");
  coordinator.shutdown();
}

void command_hold_shutdown_test() {
  CommandCoordinator coordinator(4);
  WriteLog log;
  const auto release = start_hold(coordinator, std::chrono::minutes(10));
  const auto held = logged(coordinator, log, 1);
  const auto started = std::chrono::steady_clock::now();
  coordinator.shutdown();
  require(std::chrono::steady_clock::now() - started < kPatience,
          "shutdown does not wait for a hold");
  require(log.writes() == std::vector<int>({1}),
          "shutdown drains held commands");
  (void)finished(held, "a drained command finishes");
  release();
}

void nml_command_routing_test() {
  const auto routed = [](NmlCommandKind kind, std::int32_t integer = 0) {
    NmlCommand command;
    command.kind = kind;
    command.integer = integer;
    return detail::nml_command_routing(command);
  };
  for (const auto kind :
       {NmlCommandKind::LoadToolTable, NmlCommandKind::SetTool,
        NmlCommandKind::DeleteTool, NmlCommandKind::SetJointAvailability,
        NmlCommandKind::SetSpindleAvailability}) {
    const auto routing = routed(kind);
    require(routing.task_queued, "task-queued commands start a hold");
    require(routing.priority == CommandPriority::Normal &&
                routing.hold_policy == CommandHoldPolicy::Wait,
            "task-queued commands wait for an earlier hold");
  }
  for (const auto kind : {NmlCommandKind::AbortTask, NmlCommandKind::Pause,
                          NmlCommandKind::StopProgram}) {
    const auto routing = routed(kind);
    require(routing.priority == CommandPriority::Safety, "stops are safety");
    require(!routing.task_queued, "stops do not start a hold");
  }
  for (const auto state :
       {detail::kNmlTaskStateEstop, detail::kNmlTaskStateOff}) {
    require(routed(NmlCommandKind::SetTaskState, state).priority ==
                CommandPriority::Safety,
            "E-stop and machine off are safety commands");
  }
  for (const auto state : {2, 4}) {
    const auto routing = routed(NmlCommandKind::SetTaskState, state);
    require(routing.priority == CommandPriority::Normal &&
                routing.hold_policy == CommandHoldPolicy::Wait,
            "E-stop reset and machine on wait for a hold");
  }
  for (const auto kind : {NmlCommandKind::Resume, NmlCommandKind::Step,
                          NmlCommandKind::SpindleOff}) {
    const auto routing = routed(kind);
    require(routing.priority == CommandPriority::Normal &&
                routing.hold_policy == CommandHoldPolicy::Bypass,
            "commands needed to finish held work pass a hold");
  }
  const auto ordinary = routed(NmlCommandKind::SetFeedRate);
  require(ordinary.priority == CommandPriority::Normal &&
              ordinary.hold_policy == CommandHoldPolicy::Wait &&
              !ordinary.task_queued,
          "ordinary commands wait and do not hold");
}

NmlCommandEcho task_status(int echo, std::int32_t rcs_status) {
  NmlCommandEcho status;
  status.serial = echo;
  if (rcs_status == detail::kNmlRcsStatusDone)
    status.state = NmlCommandEcho::State::Done;
  else if (rcs_status == detail::kNmlRcsStatusError)
    status.state = NmlCommandEcho::State::Error;
  return status;
}

constexpr std::int32_t kRcsExec = 2;

// A command coordinator and completion tracker around a simulated channel.
class TrackedCommands {
 public:
  explicit TrackedCommands(std::chrono::milliseconds timeout = {})
      : timeout(timeout), tracker(timeout) {
    tracker.observe(task_status(0, detail::kNmlRcsStatusDone));
  }

  CommandTicket write(int serial, bool task_queued,
                      CommandPriority priority = CommandPriority::Normal) {
    return submitted(coordinator.submit_with_context(
        [this, serial, task_queued](CommandContext& context) {
          detail::track_written_command(
              &tracker, context, serial, task_queued, timeout,
              [this] { ++completed_callbacks; },
              [this] { ++failed_callbacks; });
          // Logged once tracked, so a status the test publishes after seeing
          // this entry counts as newer than the write.
          log.add(serial);
        },
        {}, priority));
  }

  std::chrono::milliseconds timeout;
  NmlCommandCompletionTracker tracker;
  WriteLog log;
  std::atomic<int> completed_callbacks{0};
  std::atomic<int> failed_callbacks{0};
  CommandCoordinator coordinator{8};
};

void queued_command_failure_attribution_test() {
  TrackedCommands commands;
  const auto queued = commands.write(10, true);
  const auto next = commands.write(11, false);
  require(commands.log.wait_for(10), "the queued command is written");

  commands.tracker.observe(task_status(9, detail::kNmlRcsStatusDone));
  commands.tracker.observe(task_status(10, kRcsExec));
  std::this_thread::sleep_for(kSettle);
  require(commands.log.writes() == std::vector<int>({10}),
          "the next command waits for the queued command's result");

  commands.tracker.observe(task_status(10, detail::kNmlRcsStatusError));
  const auto failed = finished(queued, "the queued command resolves");
  require(failed.state == CommandState::Failed,
          "the failure belongs to the queued command");
  require(failed.error == "LinuxCNC rejected the command",
          "the queued command reports the rejection");
  require(commands.failed_callbacks == 1, "the failure callback runs");
  require(commands.log.wait_for(11), "the next command follows the result");

  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  require(finished(next, "the next command resolves").state ==
              CommandState::Completed,
          "the next command does not inherit the failure");
  require(commands.completed_callbacks == 1, "only the next command completed");
}

void queued_command_success_releases_hold_test() {
  TrackedCommands commands;
  const auto queued = commands.write(10, true);
  const auto next = commands.write(11, true);
  require(commands.log.wait_for(10), "the first queued command is written");
  commands.tracker.observe(task_status(10, detail::kNmlRcsStatusDone));
  require(finished(queued, "the first command resolves").state ==
              CommandState::Completed,
          "the first queued command completes");
  require(commands.log.wait_for(11), "the second queued command follows");
  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  require(finished(next, "the second command resolves").state ==
              CommandState::Completed,
          "the second queued command completes");
}

void queued_command_superseded_by_safety_test() {
  TrackedCommands commands;
  const auto queued = commands.write(10, true);
  require(commands.log.wait_for(10), "the queued command is written");
  const auto held = commands.write(12, false);
  const auto abort = commands.write(11, false, CommandPriority::Safety);
  require(commands.log.wait_for(11), "the safety command passes the hold");
  std::this_thread::sleep_for(kSettle);
  require(commands.log.writes() == std::vector<int>({10, 11}),
          "the normal command stays held behind the queued command");

  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  const auto superseded = finished(queued, "the overtaken command resolves");
  require(superseded.state == CommandState::Failed,
          "an overtaken queued command is not reported complete");
  require(superseded.error.find("outcome is unknown") != std::string::npos,
          "the error says the outcome is unknown");
  require(commands.completed_callbacks == 1,
          "only the safety command completed");
  require(finished(abort, "the safety command resolves").state ==
              CommandState::Completed,
          "the safety command completes");
  require(commands.log.wait_for(12), "the hold ends with the queued command");
  commands.tracker.observe(task_status(12, detail::kNmlRcsStatusDone));
  (void)finished(held, "the held command resolves");
}

void immediate_command_completes_on_later_echo_test() {
  TrackedCommands commands;
  const auto immediate = commands.write(10, false);
  const auto next = commands.write(11, false);
  require(commands.log.wait_for(11), "immediate commands do not hold");
  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  require(finished(immediate, "the first command resolves").state ==
              CommandState::Completed,
          "a later echo completes an immediate command");
  require(finished(next, "the second command resolves").state ==
              CommandState::Completed,
          "the echoed command completes");
}

void queued_command_timeout_releases_hold_test() {
  TrackedCommands commands(std::chrono::milliseconds(60));
  const auto queued = commands.write(10, true);
  const auto next = commands.write(11, false);
  require(commands.log.wait_for(10), "the queued command is written");
  commands.tracker.observe(task_status(10, kRcsExec));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  commands.tracker.tick();
  const auto timed_out = finished(queued, "the queued command times out");
  require(timed_out.state == CommandState::Failed, "a timeout is a failure");
  require(timed_out.error.find("timed out") != std::string::npos,
          "the error reports the timeout");
  require(commands.log.wait_for(11), "a timeout releases the hold");
  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  (void)finished(next, "the next command resolves");
}

void queued_command_hold_outlives_silent_status_test() {
  // With no status at all nothing resolves the command, so the hold's own
  // limit is what lets later commands continue.
  TrackedCommands commands(std::chrono::milliseconds(60));
  const auto queued = commands.write(10, true);
  const auto next = commands.write(11, false);
  require(commands.log.wait_for(10), "the queued command is written");
  require(commands.log.wait_for(11), "the hold limit releases later commands");
  commands.tracker.observe(task_status(11, detail::kNmlRcsStatusDone));
  require(finished(queued, "the expired command resolves").state ==
              CommandState::Failed,
          "a command overtaken after expiry is not reported complete");
  (void)finished(next, "the next command resolves");
}

void queued_command_channel_failure_releases_hold_test() {
  TrackedCommands commands;
  const auto queued = commands.write(10, true);
  const auto next = commands.write(11, false);
  require(commands.log.wait_for(10), "the queued command is written");
  commands.tracker.channel_failed();
  require(finished(queued, "the queued command resolves").state ==
              CommandState::Failed,
          "losing the status channel fails the queued command");
  require(commands.log.wait_for(11), "losing the channel releases the hold");
  require(
      finished(next, "the next command resolves").state == CommandState::Failed,
      "the next command also reports the lost channel");
}

void interpreter_reset_pending_test() {
  TrackedCommands commands;
  commands.tracker.observe(task_status(0, detail::kNmlRcsStatusDone));
  require(!commands.tracker.has_pending(), "nothing is pending at first");
  const auto written = commands.write(10, false);
  require(commands.log.wait_for(10), "the command is written");
  require(commands.tracker.has_pending(),
          "a written command is pending until LinuxCNC finishes it");
  commands.tracker.observe(task_status(10, detail::kNmlRcsStatusDone));
  (void)finished(written, "the written command resolves");
  require(!commands.tracker.has_pending(),
          "nothing is pending once the command resolved");
}

void command_validation_test() {
  NmlStatusSnapshot configuration;
  configuration.motion_stat.traj.joints = 3;
  configuration.motion_stat.traj.spindles = 2;

  NmlCommand command;
  command.kind = NmlCommandKind::HomeJoint;
  for (const auto valid : {-1, 0, 2}) {
    command.integer = valid;
    assert(validate_nml_command(command, &configuration));
  }
  for (const auto invalid : {-2, 3, std::numeric_limits<std::int32_t>::max()}) {
    command.integer = invalid;
    assert(!validate_nml_command(command, &configuration));
  }

  command.kind = NmlCommandKind::UnhomeJoint;
  command.integer = -2;
  assert(validate_nml_command(command, &configuration));
  command.integer = 3;
  assert(!validate_nml_command(command, &configuration));

  command.kind = NmlCommandKind::SpindleOff;
  for (const auto valid : {0, 1}) {
    command.integer = valid;
    assert(validate_nml_command(command, &configuration));
  }
  for (const auto invalid :
       {-2, -1, 2, std::numeric_limits<std::int32_t>::max()}) {
    command.integer = invalid;
    assert(!validate_nml_command(command, &configuration));
  }
  assert(validate_nml_command(command, nullptr).code ==
         NmlCommandValidationCode::StatusUnavailable);

  command = {};
  command.kind = NmlCommandKind::SetJointAvailability;
  command.integer = 2;
  command.boolean = true;
  assert(validate_nml_command(command, &configuration));
  command.integer = 3;
  assert(!validate_nml_command(command, &configuration));

  command.kind = NmlCommandKind::SetSpindleAvailability;
  command.integer = 1;
  command.boolean = false;
  assert(validate_nml_command(command, &configuration));
  command.integer = -1;
  assert(!validate_nml_command(command, &configuration));

  const std::array<double, 3> malformed{
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity()};
  const std::array<NmlCommandKind, 7> physical_commands{
      NmlCommandKind::SetMaxVelocity,      NmlCommandKind::SetFeedRate,
      NmlCommandKind::SetRapidRate,        NmlCommandKind::SetSpindleOverride,
      NmlCommandKind::SetMinPositionLimit, NmlCommandKind::SpindleOn,
      NmlCommandKind::SetAnalogOutput};
  for (const auto kind : physical_commands) {
    command = {};
    command.kind = kind;
    command.integer = 0;
    command.boolean = true;
    for (const auto value : malformed) {
      command.number = value;
      assert(!validate_nml_command(command, &configuration));
    }
  }

  command = {};
  command.kind = NmlCommandKind::SetTool;
  command.tool.tool_no = 1;
  command.tool.has_offset = true;
  command.tool.offset_values = command.tool.offset.values.size();
  command.tool.offset.values.back() = std::numeric_limits<double>::quiet_NaN();
  assert(!validate_nml_command(command, &configuration));

  command = {};
  command.kind = NmlCommandKind::SetTool;
  assert(!validate_nml_command(command, &configuration));
  command.tool.tool_no = 7;
  command.tool.has_pocket_no = true;
  command.tool.pocket_no = 42;
  command.tool.has_diameter = true;
  command.tool.diameter = 5.0;
  assert(validate_nml_command(command, &configuration));
  command.tool.pocket_no = 0;
  assert(!validate_nml_command(command, &configuration));
  command.tool.pocket_no = 42;
  command.tool.diameter = -1.0;
  assert(!validate_nml_command(command, &configuration));
  command.tool.diameter = 5.0;
  command.tool.has_orientation = true;
  command.tool.orientation = 10;
  assert(!validate_nml_command(command, &configuration));
  command.tool.orientation = 3;
  command.tool.has_comment = true;
  command.tool.comment = "unsafe\nT8 P8";
  assert(!validate_nml_command(command, &configuration));

  command = {};
  command.kind = NmlCommandKind::DeleteTool;
  assert(!validate_nml_command(command, &configuration));
  command.integer = 7;
  assert(validate_nml_command(command, &configuration));
}

void nml_serial_wrap_test() {
  constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
  constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
  static_assert(detail::nml_serial_after(minimum, maximum));
  static_assert(!detail::nml_serial_after(maximum, minimum));
  static_assert(detail::nml_serial_after(42, 41));
  static_assert(!detail::nml_serial_after(41, 42));
}

PositionSample position(double x, std::int32_t motion = 0) {
  PositionSample sample;
  sample.coordinates[0] = x;
  sample.motion_type = motion;
  return sample;
}

PositionSample position_xy(double x, double y, std::int32_t motion = 0) {
  auto sample = position(x, motion);
  sample.coordinates[1] = y;
  return sample;
}

void position_cursor_generation_and_replacement_test() {
  PositionHistory history(2, 0.1);
  assert(history.next_sequence() == 0);
  assert(history.append(position(1.0)));
  auto near_duplicate = position(1.05);
  assert(!history.append(near_duplicate));
  near_duplicate.motion_type = 7;
  assert(history.append(near_duplicate));
  assert(history.append(position(3.0)));
  assert(history.size() == 2);

  const auto initial = history.snapshot();
  assert(initial.reset);
  assert(initial.first_sequence == 1);
  assert(initial.oldest_sequence == 1);
  assert(initial.next_sequence == 3);
  assert(initial.packed.size() == 2 * kPositionStride);
  assert(initial.packed[0] == 1.05);
  assert(initial.packed[kPositionStride] == 3.0);

  const auto rolled = history.since(0);
  assert(rolled.reset);
  assert(rolled.first_sequence == 1);
  assert(rolled.packed.size() == 2 * kPositionStride);
  const auto bounded = history.since(1, 1);
  assert(!bounded.reset);
  assert(bounded.first_sequence == 1);
  assert(bounded.packed.size() == kPositionStride);
  assert(bounded.packed[0] == 1.05);
  assert(history.since(history.next_sequence()).packed.empty());

  const auto old_generation = initial.generation;
  history.configure(1);
  const auto configured = history.snapshot();
  assert(configured.generation != old_generation);
  assert(configured.first_sequence == 2);
  assert(configured.packed.size() == kPositionStride);
  assert(history.since(configured.first_sequence, 0, old_generation).reset);

  history.clear();
  const auto cleared = history.snapshot();
  assert(cleared.reset);
  assert(cleared.generation != configured.generation);
  assert(cleared.first_sequence == cleared.next_sequence);
  assert(cleared.packed.empty());
  assert(history.since(history.next_sequence(), 0, cleared.generation)
             .packed.empty());

  // Capacity eviction keeps a synchronized consumer on deltas and tells it
  // which prefix the history no longer retains.
  PositionHistory retained(2);
  assert(retained.append(position(1.0, 1)));
  assert(retained.append(position(2.0, 2)));
  const auto synchronized = retained.snapshot();
  assert(synchronized.oldest_sequence == 0);
  assert(retained.append(position(3.0, 3)));
  const auto evicted =
      retained.since(synchronized.next_sequence, 0, synchronized.generation);
  assert(!evicted.reset);
  assert(evicted.generation == synchronized.generation);
  assert(evicted.oldest_sequence == 1);
  assert(evicted.first_sequence == 2);
  assert(evicted.next_sequence == 3);
  assert(evicted.packed.size() == kPositionStride);
  assert(evicted.packed[0] == 3.0);

  PositionHistory non_finite_history(8);
  auto finite = position(1.0);
  auto invalid = finite;
  invalid.coordinates[0] = std::numeric_limits<double>::quiet_NaN();
  assert(non_finite_history.append(finite));

  PositionHistory large(100000);
  for (int index = 0; index < 100001; ++index)
    assert(large.append(position(static_cast<double>(index), index % 2)));
  assert(large.size() == 100000);
  assert(non_finite_history.append(invalid));
  assert(!non_finite_history.append(invalid));
  assert(non_finite_history.append(finite));
}

void position_collinear_compaction_test() {
  PositionHistory straight(16);
  assert(straight.append(position(0.0)));
  assert(straight.append(position(1.0)));
  assert(straight.append(position(2.0)));
  assert(straight.size() == 2);
  const auto compacted = straight.snapshot();
  assert(compacted.first_sequence == 0);
  assert(compacted.next_sequence == 2);
  assert(compacted.packed.size() == 2 * kPositionStride);
  assert(compacted.packed[0] == 0.0);
  assert(compacted.packed[kPositionStride] == 2.0);

  // Once a read returns the tail, a further collinear sample starts a new
  // point instead of changing the delivered one. A tail no read has returned
  // still absorbs collinear samples and keeps its sequence.
  assert(straight.append(position(3.0)));
  assert(straight.append(position(4.0)));
  assert(straight.size() == 3);
  const auto extended = straight.since(compacted.next_sequence);
  assert(!extended.reset);
  assert(extended.first_sequence == 2);
  assert(extended.next_sequence == 3);
  assert(extended.packed.size() == kPositionStride);
  assert(extended.packed[0] == 4.0);
  assert(straight.append(position(5.0)));
  assert(straight.size() == 4);
  const auto after_seal = straight.since(extended.next_sequence);
  assert(after_seal.first_sequence == 3);
  assert(after_seal.next_sequence == 4);
  assert(after_seal.packed.size() == kPositionStride);
  assert(after_seal.packed[0] == 5.0);
  // Every delivered point is still reported with its original sequence.
  const auto whole = straight.snapshot();
  assert(whole.first_sequence == 0);
  assert(whole.next_sequence == 4);
  assert(whole.packed.size() == 4 * kPositionStride);
  assert(whole.packed[kPositionStride] == 2.0);
  assert(whole.packed[2 * kPositionStride] == 4.0);

  // A bounded read seals only the points it returned.
  PositionHistory bounded(16);
  assert(bounded.append(position_xy(0.0, 0.0)));
  assert(bounded.append(position_xy(1.0, 0.0)));
  assert(bounded.append(position_xy(1.0, 1.0)));
  assert(bounded.since(0, 1).packed.size() == kPositionStride);
  assert(bounded.append(position_xy(1.0, 2.0)));
  assert(bounded.size() == 3);
  const auto unreturned = bounded.since(1);
  assert(unreturned.first_sequence == 1);
  assert(unreturned.next_sequence == 3);
  assert(unreturned.packed.size() == 2 * kPositionStride);
  assert(unreturned.packed[kPositionStride + 1] == 2.0);

  PositionHistory corner(16);
  assert(corner.append(position_xy(0.0, 0.0)));
  assert(corner.append(position_xy(1.0, 0.0)));
  assert(corner.append(position_xy(1.0, 1.0)));
  assert(corner.size() == 3);

  PositionHistory rotary(16);
  auto rotary_start = position(0.0);
  auto rotary_middle = rotary_start;
  auto rotary_end = rotary_start;
  rotary_middle.coordinates[3] = 1.0;
  assert(rotary.append(rotary_start));
  assert(rotary.append(rotary_middle));
  assert(rotary.append(rotary_end));
  assert(rotary.size() == 3);

  PositionHistory arc(256);
  for (int step = 0; step <= 100; ++step) {
    const double angle = static_cast<double>(step) * 0.01;
    assert(arc.append(position_xy(std::cos(angle), std::sin(angle), 3)));
  }
  assert(arc.size() > 2);
  assert(arc.size() < 101);
}

#ifdef LINUXCNC_GRPC_HAS_RS274
void preview_non_finite_rejection_test() {
  gcode::ParseContext context;
  gcode::FeedOp operation;
  operation.pos.x = std::numeric_limits<double>::infinity();
  bool rejected = false;
  try {
    context.addOperation(gcode::Operation{operation});
  } catch (const std::domain_error&) {
    rejected = true;
  }
  assert(rejected);
  assert(context.operations.empty());
}
#endif

void hal_value_telemetry_test() {
  HalValueTelemetry telemetry(2);
  std::vector<HalTelemetryResolvedItem> items{
      {{HalTelemetryItemKind::Pin, "test.bit"}, HalTelemetryType::Bit},
      {{HalTelemetryItemKind::Signal, "test.u64"}, HalTelemetryType::U64}};
  const auto created = telemetry.create(items, std::chrono::milliseconds(50));
  assert(created && created->revision == 1 && created->bindings.size() == 2);
  const auto token = created->websocket_path.substr(
      created->websocket_path.find_last_of('/') + 1);
  const auto claimed = telemetry.claim(token);
  assert(claimed && *claimed == created->subscription_id);
  assert(!telemetry.claim(token));
  const auto due = telemetry.due(std::chrono::steady_clock::now());
  assert(due.size() == 1 && due[0].bindings.size() == 2);
  telemetry.publish(created->subscription_id, 1,
                    {HalTelemetryValue{true},
                     HalTelemetryValue{std::uint64_t{0xffffffffffffffffULL}}});
  const auto snapshot = telemetry.snapshot(created->subscription_id);
  assert(snapshot && snapshot->sampled && snapshot->sequence == 1);
  std::vector<HalTelemetryResolvedItem> changed{
      items[0],
      {{HalTelemetryItemKind::Param, "test.s32"}, HalTelemetryType::S32}};
  const auto updated = telemetry.update(created->subscription_id, 1, changed,
                                        std::chrono::milliseconds(100));
  assert(updated && updated->revision == 2 && updated->bindings.size() == 2);
  assert(updated->websocket_path == created->websocket_path);
  assert(updated->bindings[0].slot == created->bindings[0].slot);
  assert(updated->bindings[1].slot > created->bindings[1].slot);
  assert(telemetry.detach(created->subscription_id));
  assert(telemetry.claim(token) == created->subscription_id);
  assert(telemetry.erase(created->subscription_id));
  assert(!telemetry.snapshot(created->subscription_id));

  HalValueTelemetry budgeted(4);
  std::vector<HalTelemetryResolvedItem> full_budget;
  full_budget.reserve(1024);
  for (std::size_t index = 0; index < 1024; ++index)
    full_budget.push_back(
        {{HalTelemetryItemKind::Pin, "budget." + std::to_string(index)},
         HalTelemetryType::Bit});
  const auto full = budgeted.create(full_budget, std::chrono::milliseconds(50));
  assert(full);
  assert(budgeted.create({full_budget.front()}, std::chrono::milliseconds(50)));
  assert(!budgeted.create(
      {{{HalTelemetryItemKind::Pin, "budget.extra"}, HalTelemetryType::Bit}},
      std::chrono::milliseconds(50)));
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void write_workspace_archive(
    const fs::path& path,
    const std::vector<std::pair<std::string, std::string>>& files,
    bool compressed = true) {
  auto* writer = archive_write_new();
  assert(writer);
  assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
  if (compressed) assert(archive_write_add_filter_zstd(writer) == ARCHIVE_OK);
  assert(archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK);
  for (const auto& [name, contents] : files) {
    auto* entry = archive_entry_new();
    assert(entry);
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
    assert(archive_write_header(writer, entry) == ARCHIVE_OK);
    assert(archive_write_data(writer, contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    archive_entry_free(entry);
  }
  assert(archive_write_close(writer) == ARCHIVE_OK);
  assert(archive_write_free(writer) == ARCHIVE_OK);
}

void write_symlink_archive(const fs::path& path) {
  auto* writer = archive_write_new();
  assert(writer);
  assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
  assert(archive_write_add_filter_zstd(writer) == ARCHIVE_OK);
  assert(archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK);
  auto* entry = archive_entry_new();
  assert(entry);
  archive_entry_set_pathname(entry, "linked.ngc");
  archive_entry_set_filetype(entry, AE_IFLNK);
  archive_entry_set_perm(entry, 0777);
  archive_entry_set_symlink(entry, "../outside.ngc");
  assert(archive_write_header(writer, entry) == ARCHIVE_OK);
  archive_entry_free(entry);
  assert(archive_write_close(writer) == ARCHIVE_OK);
  assert(archive_write_free(writer) == ARCHIVE_OK);
}

void workspace_restart_cleanup_test() {
  const auto base =
      fs::temp_directory_path() /
      ("linuxcnc-grpc-workspace-restart-tests-" + std::to_string(::getpid()));
  const auto root = base / "workspaces";
  const auto active = base / "active-program";
  std::error_code error;
  fs::remove_all(base, error);
  fs::create_directories(root / ".uploads/orphan", error);
  fs::create_directories(active, error);
  fs::permissions(base, fs::perms::owner_all, fs::perm_options::replace, error);
  fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, error);
  assert(!error);
  {
    std::ofstream stale(active / "stale.ngc");
    stale << "stale";
  }
  {
    ProgramWorkspaceStore store(root, active);
    assert(fs::is_symlink(fs::symlink_status(active)));
    assert(!fs::exists(active / "stale.ngc"));
    assert(fs::is_empty(store.staging_root()));
  }
  fs::remove_all(base, error);
  assert(!error);
}

std::string publish_test_workspace(ProgramWorkspaceStore& store,
                                   const std::string& staging_name) {
  std::error_code error;
  const auto revision = store.staging_root() / staging_name / "revision";
  fs::create_directories(revision, error);
  assert(!error);
  {
    std::ofstream program(revision / "program.ngc");
    program << "M2\n";
  }
  std::string id;
  assert(store.publish_revision(revision, 3, 1, &id) ==
         WorkspacePublishStatus::Ok);
  return id;
}

void workspace_expiration_and_recovery_test() {
  const auto base =
      fs::temp_directory_path() /
      ("linuxcnc-grpc-workspace-recovery-tests-" + std::to_string(::getpid()));
  const auto root = base / "workspaces";
  const auto active = base / "active-program";
  std::error_code error;
  fs::remove_all(base, error);

  std::string expired_id;
  {
    ProgramWorkspaceStore store(root, active);
    expired_id = publish_test_workspace(store, "expires");
  }
  fs::last_write_time(
      root / expired_id,
      fs::file_time_type::clock::now() - std::chrono::seconds(1), error);
  assert(!error);
  {
    ProgramWorkspaceStore restarted(root, active);
    fs::path resolved;
    assert(!restarted.pin_entry(expired_id, "program.ngc", &resolved));
    assert(restarted.prune_expired() == 1);
    assert(!fs::exists(root / expired_id));
  }

  std::string active_id;
  std::string interrupted_id;
  {
    ProgramWorkspaceStore store(root, active);
    active_id = publish_test_workspace(store, "active");
    interrupted_id = publish_test_workspace(store, "interrupted");
    fs::path resolved;
    assert(store.pin_entry(active_id, "program.ngc", &resolved));
    auto activation =
        store.stage(active_id, "program.ngc", [](const fs::path&) {});
    assert(activation && activation->commit());
    activation->complete();
    fs::create_directory_symlink(root / interrupted_id,
                                 base / ".active-program.next-crash", error);
    assert(!error);
  }
  {
    ProgramWorkspaceStore restarted(root, active);
    assert(!restarted.erase(active_id));
    assert(!restarted.erase(interrupted_id));
    assert(restarted.release_recovered_leases());
    assert(!fs::exists(base / ".active-program.next-crash"));
    restarted.clear_active_link();
    assert(restarted.erase(active_id));
    assert(restarted.erase(interrupted_id));
  }

  fs::remove_all(base, error);
  assert(!error);
}

void workspace_path_safety_test() {
  const auto base =
      fs::temp_directory_path() /
      ("linuxcnc-grpc-workspace-path-tests-" + std::to_string(::getpid()));
  std::error_code error;
  fs::remove_all(base, error);
  fs::create_directories(base / "insecure", error);
  assert(!error);
  fs::permissions(base / "insecure", fs::perms::all, fs::perm_options::replace,
                  error);
  assert(!error);
  bool rejected = false;
  try {
    ProgramWorkspaceStore store(base / "insecure", base / "active");
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  rejected = false;
  try {
    ProgramWorkspaceStore store(base / "overlap", base / "overlap/active");
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
  fs::remove_all(base, error);
  assert(!error);
}

void workspace_active_parent_permissions_test() {
  const auto base =
      fs::temp_directory_path() /
      ("linuxcnc-grpc-workspace-parent-tests-" + std::to_string(::getpid()));
  const auto root = base / "workspaces";
  const auto active = base / "active";
  std::error_code error;
  fs::remove_all(base, error);

  const auto previous_umask = ::umask(0002);
  ProgramWorkspaceStore store(root, active);
  ::umask(previous_umask);

  struct stat status{};
  assert(::lstat(active.parent_path().c_str(), &status) == 0);
  assert((status.st_mode & (S_IWGRP | S_IWOTH)) == 0);
  fs::remove_all(base, error);
  assert(!error);
}

void workspace_traversal_quota_ttl_and_materialization_test() {
  const auto base =
      fs::temp_directory_path() /
      ("linuxcnc-grpc-workspace-archive-tests-" + std::to_string(::getpid()));
  std::error_code error;
  fs::remove_all(base, error);

  ProgramWorkspaceStore store(
      base / "workspaces", base / "active-program",
      WorkspaceLimits{64, 128, std::chrono::hours(24), 2, 4});

  const auto first_upload = store.staging_root() / "first";
  const auto first_revision = first_upload / "revision";
  fs::create_directories(first_revision, error);
  assert(!error);
  const auto first_archive = first_upload / "workspace.tar.zst";
  write_workspace_archive(first_archive, {{"program/main.ngc", "G0 X1\n"},
                                          {"sub/companion.ngc", "M2\n"}});
  const auto first_extract = extract_workspace_archive(
      first_archive, first_revision, WorkspaceArchiveLimits{64, 4, 128});
  assert(first_extract.status == WorkspaceArchiveStatus::Ok);
  assert(first_extract.extracted_bytes == 9);
  assert(first_extract.entries == 2);

  std::string first_id;
  assert(store.publish_revision(first_revision, first_extract.extracted_bytes,
                                first_extract.entries,
                                &first_id) == WorkspacePublishStatus::Ok);
  fs::path resolved;
  assert(!store.pin_entry(first_id, "../main.ngc", &resolved));
  assert(store.pin_entry(first_id, "program/main.ngc", &resolved));
  assert(read_file(resolved) == "G0 X1\n");
  auto first_activation =
      store.stage(first_id, "program/main.ngc", [](const fs::path& path) {
        std::error_code cleanup_error;
        fs::remove(path, cleanup_error);
      });
  assert(first_activation && first_activation->commit());
  first_activation->complete();
  assert(fs::is_symlink(fs::symlink_status(store.active_directory())));
  assert(read_file(store.active_directory() / "program/main.ngc") == "G0 X1\n");

  const auto second_upload = store.staging_root() / "second";
  const auto second_revision = second_upload / "revision";
  fs::create_directories(second_revision, error);
  assert(!error);
  const auto second_archive = second_upload / "workspace.tar.zst";
  write_workspace_archive(second_archive, {{"program/main.ngc", "G1 X2\n"}});
  const auto second_extract = extract_workspace_archive(
      second_archive, second_revision, WorkspaceArchiveLimits{64, 4, 128});
  assert(second_extract.status == WorkspaceArchiveStatus::Ok);
  std::string second_id;
  assert(store.publish_revision(second_revision, second_extract.extracted_bytes,
                                second_extract.entries,
                                &second_id) == WorkspacePublishStatus::Ok);
  assert(store.pin_entry(second_id, "program/main.ngc", &resolved));

  auto rejected_activation =
      store.stage(second_id, "program/main.ngc", [](const fs::path& path) {
        std::error_code cleanup_error;
        fs::remove(path, cleanup_error);
      });
  assert(rejected_activation && rejected_activation->commit());
  assert(read_file(store.active_directory() / "program/main.ngc") == "G1 X2\n");
  rejected_activation->rollback();
  assert(read_file(store.active_directory() / "program/main.ngc") == "G0 X1\n");
  assert(store.unpin_entry(second_id));

  assert(!store.erase(first_id));
  assert(store.unpin_entry(first_id));
  assert(store.erase(first_id));
  assert(store.erase(second_id));

  const auto unsafe_root = base / "unsafe";
  fs::create_directories(unsafe_root / "revision", error);
  write_workspace_archive(unsafe_root / "traversal.tar.zst",
                          {{"../escape.ngc", "bad"}});
  const auto unsafe = extract_workspace_archive(
      unsafe_root / "traversal.tar.zst", unsafe_root / "revision",
      WorkspaceArchiveLimits{64, 4, 128});
  assert(unsafe.status == WorkspaceArchiveStatus::Invalid);
  assert(!fs::exists(base / "escape.ngc"));

  fs::remove_all(unsafe_root / "revision", error);
  fs::create_directory(unsafe_root / "revision", error);
  write_workspace_archive(unsafe_root / "plain.tar", {{"program.ngc", "M2\n"}},
                          false);
  const auto plain = extract_workspace_archive(
      unsafe_root / "plain.tar", unsafe_root / "revision",
      WorkspaceArchiveLimits{64, 4, 128});
  assert(plain.status == WorkspaceArchiveStatus::Invalid);

  fs::remove_all(unsafe_root / "revision", error);
  fs::create_directory(unsafe_root / "revision", error);
  write_symlink_archive(unsafe_root / "link.tar.zst");
  const auto linked = extract_workspace_archive(
      unsafe_root / "link.tar.zst", unsafe_root / "revision",
      WorkspaceArchiveLimits{64, 4, 128});
  assert(linked.status == WorkspaceArchiveStatus::Invalid);

  fs::remove_all(unsafe_root / "revision", error);
  fs::create_directory(unsafe_root / "revision", error);
  write_workspace_archive(unsafe_root / "entries.tar.zst",
                          {{"one.ngc", "1"}, {"two.ngc", "2"}});
  const auto entries = extract_workspace_archive(
      unsafe_root / "entries.tar.zst", unsafe_root / "revision",
      WorkspaceArchiveLimits{64, 1, 128});
  assert(entries.status == WorkspaceArchiveStatus::ResourceExhausted);

  fs::remove_all(unsafe_root / "revision", error);
  fs::create_directory(unsafe_root / "revision", error);
  write_workspace_archive(unsafe_root / "large.tar.zst",
                          {{"program.ngc", "12345"}});
  const auto large = extract_workspace_archive(
      unsafe_root / "large.tar.zst", unsafe_root / "revision",
      WorkspaceArchiveLimits{4, 4, 128});
  assert(large.status == WorkspaceArchiveStatus::ResourceExhausted);

  fs::remove_all(base, error);
  assert(!error);
}

}  // namespace

int main() {
  command_queue_bounds_and_wait_test();
  command_cancellation_and_acceptance_test();
  command_cancellation_before_worker_start_test();
  command_failure_wait_test();
  command_priority_and_reserved_capacity_test();
  deferred_command_completion_test();
  command_hold_keeps_later_commands_in_order_test();
  command_hold_bypass_test();
  command_hold_bypass_keeps_order_without_hold_test();
  command_hold_expiry_test();
  command_hold_shutdown_test();
  nml_command_routing_test();
  queued_command_failure_attribution_test();
  queued_command_success_releases_hold_test();
  queued_command_superseded_by_safety_test();
  immediate_command_completes_on_later_echo_test();
  queued_command_timeout_releases_hold_test();
  queued_command_hold_outlives_silent_status_test();
  queued_command_channel_failure_releases_hold_test();
  interpreter_reset_pending_test();
  command_validation_test();
  nml_serial_wrap_test();
  position_cursor_generation_and_replacement_test();
  position_collinear_compaction_test();
#ifdef LINUXCNC_GRPC_HAS_RS274
  preview_non_finite_rejection_test();
#endif
  hal_value_telemetry_test();
  workspace_restart_cleanup_test();
  workspace_expiration_and_recovery_test();
  workspace_path_safety_test();
  workspace_active_parent_permissions_test();
  workspace_traversal_quota_ttl_and_materialization_test();
  return 0;
}

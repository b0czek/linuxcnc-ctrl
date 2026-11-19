#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace linuxcnc::server {

enum class CommandWaitPolicy {
  Accepted,
  Completed,
};

enum class CommandState {
  Queued,
  Accepted,
  Completed,
  Failed,
};

enum class CommandPriority {
  Normal,
  Safety,
  // Safety, and also cancels every normal-priority command still queued when
  // it is submitted. For commands after which earlier work must not run.
  Interrupt,
};

// Whether a normal-priority command waits while the normal queue is held.
// Safety-priority commands never wait for a hold.
enum class CommandHoldPolicy {
  Wait,
  Bypass,
};

enum class CommandSubmitStatus {
  Submitted,
  QueueFull,
  Stopped,
};

struct CommandResult {
  // Internal sequence assigned when work enters the daemon coordinator.
  std::uint64_t sequence = 0;
  // Identifier assigned by the command channel when it accepts the command.
  // Zero means the action did not produce a channel command.
  std::uint64_t accepted_sequence = 0;
  CommandState state = CommandState::Queued;
  std::string error;
};

struct CommandContext {
  // NML adapters call this only after the command has been accepted by the
  // command channel, passing the channel's identifier. An RPC cancellation
  // never invokes it or removes work.
  std::function<void(std::uint64_t)> mark_accepted;
  std::stop_token stop_token;
  // A writer calls defer_completion after a successful external write. The
  // status multiplexer later resolves the ticket through these callbacks.
  std::function<void()> defer_completion;
  std::function<void()> mark_completed;
  std::function<void(std::string)> mark_failed;
  // Stops later normal-priority commands from starting until the returned
  // callable runs or the limit passes, whichever is first. The callable may run
  // on any thread, more than once, and after a newer hold replaced this one.
  std::function<std::function<void()>(std::chrono::milliseconds)>
      hold_normal_commands;
};

// A command is deliberately a native callable.  The gRPC layer translates a
// protobuf oneof into this object, while this coordinator remains unaware of
// protobuf and can be exercised without LinuxCNC or a network connection.
using CommandAction = std::function<void()>;
using ContextCommandAction = std::function<void(CommandContext&)>;
// Runs instead of the action when an Interrupt command cancels a queued
// command, so the command can release what it reserved when submitted.
using CommandCancelHandler = std::function<void()>;

class CommandTicket {
 public:
  using Observer = std::function<void(const CommandResult&)>;

  CommandTicket() = default;

  std::uint64_t sequence() const noexcept;
  bool wait_for(CommandWaitPolicy policy, std::chrono::milliseconds timeout,
                CommandResult* result) const;
  bool wait_for(std::chrono::milliseconds timeout, CommandResult* result) const;
  CommandResult wait() const;
  // Registers a one-shot notification without occupying a worker. The
  // observer is invoked immediately when the requested state is already
  // available, otherwise by the command worker after releasing state locks.
  bool observe(CommandWaitPolicy policy, Observer observer) const;

 private:
  struct State {
    struct PendingObserver {
      CommandWaitPolicy policy;
      Observer callback;
    };
    mutable std::mutex mutex;
    std::condition_variable condition;
    CommandResult result;
    std::vector<PendingObserver> observers;
  };

  explicit CommandTicket(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  friend class CommandCoordinator;
  std::shared_ptr<State> state_;
};

struct CommandSubmission {
  CommandSubmitStatus status = CommandSubmitStatus::Stopped;
  CommandTicket ticket;

  explicit operator bool() const noexcept {
    return status == CommandSubmitStatus::Submitted;
  }
};

class CommandCoordinator {
 public:
  explicit CommandCoordinator(std::size_t capacity = 128,
                              std::size_t safety_capacity = 8);
  ~CommandCoordinator();

  CommandCoordinator(const CommandCoordinator&) = delete;
  CommandCoordinator& operator=(const CommandCoordinator&) = delete;

  // submit() only queues the action.  The action is executed serially by the
  // coordinator worker, and cancellation of an RPC wait never removes an
  // already accepted action from this queue.
  CommandSubmission submit(CommandAction action,
                           CommandPriority priority = CommandPriority::Normal);
  CommandSubmission submit_with_context(ContextCommandAction action);
  CommandSubmission submit_with_context(
      ContextCommandAction action, const std::stop_token& stop_token,
      CommandPriority priority = CommandPriority::Normal,
      CommandHoldPolicy hold_policy = CommandHoldPolicy::Wait,
      CommandCancelHandler on_cancelled = {});
  // Stops the worker after it drains both queues. A hold does not delay this:
  // nothing resolves a hold once the service stops polling LinuxCNC.
  void shutdown();
  std::size_t queued() const;

 private:
  struct Item {
    std::uint64_t sequence = 0;
    CommandAction action;
    ContextCommandAction context_action;
    std::shared_ptr<CommandTicket::State> state;
    bool bypass_hold = false;
    CommandCancelHandler on_cancelled;
  };

  void run();
  // Queues `item`, cancelling queued normal commands first for an Interrupt.
  CommandSubmitStatus enqueue(Item item, CommandPriority priority);
  static void cancel(Item& item);
  // Moves the next runnable command into `item`. Requires mutex_.
  bool take_next(Item* item);
  std::function<void()> hold_normal_commands(std::chrono::milliseconds limit);
  void release_hold(std::uint64_t generation);
  static void transition(const std::shared_ptr<CommandTicket::State>& state,
                         CommandState result, std::string error = {},
                         std::uint64_t accepted_sequence = 0);

  const std::size_t capacity_;
  const std::size_t safety_capacity_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Item> queue_;
  std::deque<Item> safety_queue_;
  std::uint64_t next_sequence_ = 1;
  bool hold_active_ = false;
  std::uint64_t hold_generation_ = 0;
  std::chrono::steady_clock::time_point hold_deadline_{};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace linuxcnc::server

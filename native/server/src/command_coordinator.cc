#include "linuxcnc_grpc/command_coordinator.hpp"

#include <algorithm>
#include <stdexcept>

namespace linuxcnc::server {

std::uint64_t CommandTicket::sequence() const noexcept {
  if (!state_) return 0;
  std::lock_guard lock(state_->mutex);
  return state_->result.sequence;
}

bool CommandTicket::wait_for(std::chrono::milliseconds timeout,
                             CommandResult* result) const {
  return wait_for(CommandWaitPolicy::Completed, timeout, result);
}

bool CommandTicket::wait_for(CommandWaitPolicy policy,
                             std::chrono::milliseconds timeout,
                             CommandResult* result) const {
  if (!state_) return false;
  std::unique_lock lock(state_->mutex);
  const bool ready = state_->condition.wait_for(lock, timeout, [this, policy] {
    if (policy == CommandWaitPolicy::Accepted) {
      return state_->result.state == CommandState::Accepted ||
             state_->result.state == CommandState::Completed ||
             state_->result.state == CommandState::Failed;
    }
    return state_->result.state == CommandState::Completed ||
           state_->result.state == CommandState::Failed;
  });
  if (result) *result = state_->result;
  return ready;
}

CommandResult CommandTicket::wait() const {
  if (!state_) return {};
  std::unique_lock lock(state_->mutex);
  state_->condition.wait(lock, [this] {
    return state_->result.state == CommandState::Completed ||
           state_->result.state == CommandState::Failed;
  });
  return state_->result;
}

bool CommandTicket::observe(CommandWaitPolicy policy, Observer observer) const {
  if (!state_ || !observer) return false;
  CommandResult ready_result;
  bool ready = false;
  {
    std::lock_guard lock(state_->mutex);
    ready = state_->result.state == CommandState::Failed ||
            state_->result.state == CommandState::Completed ||
            (policy == CommandWaitPolicy::Accepted &&
             state_->result.state == CommandState::Accepted);
    if (!ready) {
      state_->observers.push_back({policy, std::move(observer)});
      return true;
    }
    ready_result = state_->result;
  }
  observer(ready_result);
  return true;
}

CommandCoordinator::CommandCoordinator(std::size_t capacity,
                                       std::size_t safety_capacity)
    : capacity_(capacity == 0 ? 1 : capacity),
      safety_capacity_(safety_capacity == 0 ? 1 : safety_capacity),
      worker_(&CommandCoordinator::run, this) {}

CommandCoordinator::~CommandCoordinator() { shutdown(); }

CommandSubmission CommandCoordinator::submit(CommandAction action,
                                             CommandPriority priority) {
  if (!action) throw std::invalid_argument("command action must not be empty");
  auto state = std::make_shared<CommandTicket::State>();
  const auto status =
      enqueue(Item{0, std::move(action), {}, state, false, {}}, priority);
  if (status != CommandSubmitStatus::Submitted) return {status, {}};
  return {status, CommandTicket(std::move(state))};
}

CommandSubmission CommandCoordinator::submit_with_context(
    ContextCommandAction action) {
  return submit_with_context(std::move(action), {}, CommandPriority::Normal,
                             CommandHoldPolicy::Wait);
}

CommandSubmission CommandCoordinator::submit_with_context(
    ContextCommandAction action, const std::stop_token& stop_token,
    CommandPriority priority, CommandHoldPolicy hold_policy,
    CommandCancelHandler on_cancelled) {
  if (!action) throw std::invalid_argument("command action must not be empty");
  auto state = std::make_shared<CommandTicket::State>();
  const auto status = enqueue(
      Item{0,
           {},
           [action = std::move(action), stop_token](CommandContext& context) {
             context.stop_token = stop_token;
             action(context);
           },
           state,
           hold_policy == CommandHoldPolicy::Bypass,
           std::move(on_cancelled)},
      priority);
  if (status != CommandSubmitStatus::Submitted) return {status, {}};
  return {status, CommandTicket(std::move(state))};
}

CommandSubmitStatus CommandCoordinator::enqueue(Item item,
                                                CommandPriority priority) {
  std::deque<Item> cancelled;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return CommandSubmitStatus::Stopped;
    const bool safety = priority != CommandPriority::Normal;
    auto& queue = safety ? safety_queue_ : queue_;
    if (queue.size() >= (safety ? safety_capacity_ : capacity_))
      return CommandSubmitStatus::QueueFull;
    // Under the same lock that queues the interrupt, so exactly the commands
    // submitted before it are cancelled and later ones are kept.
    if (priority == CommandPriority::Interrupt) cancelled.swap(queue_);
    item.sequence = next_sequence_++;
    item.state->result.sequence = item.sequence;
    queue.push_back(std::move(item));
  }
  condition_.notify_one();
  for (auto& queued : cancelled) cancel(queued);
  return CommandSubmitStatus::Submitted;
}

void CommandCoordinator::cancel(Item& item) {
  std::string error =
      "cancelled before reaching LinuxCNC by a later abort, E-stop or "
      "machine off";
  try {
    if (item.on_cancelled) item.on_cancelled();
  } catch (const std::exception& cleanup_error) {
    error += "; cleanup failed: ";
    error += cleanup_error.what();
  } catch (...) {
    error += "; cleanup failed";
  }
  transition(item.state, CommandState::Failed, std::move(error));
}

void CommandCoordinator::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::size_t CommandCoordinator::queued() const {
  std::lock_guard lock(mutex_);
  return queue_.size() + safety_queue_.size();
}

bool CommandCoordinator::take_next(Item* item) {
  if (hold_active_ && std::chrono::steady_clock::now() >= hold_deadline_)
    hold_active_ = false;
  if (!safety_queue_.empty()) {
    *item = std::move(safety_queue_.front());
    safety_queue_.pop_front();
    return true;
  }
  auto next = queue_.begin();
  if (hold_active_ && !stopping_) {
    next = std::find_if(queue_.begin(), queue_.end(),
                        [](const Item& queued) { return queued.bypass_hold; });
  }
  if (next == queue_.end()) return false;
  *item = std::move(*next);
  queue_.erase(next);
  return true;
}

std::function<void()> CommandCoordinator::hold_normal_commands(
    std::chrono::milliseconds limit) {
  std::lock_guard lock(mutex_);
  hold_active_ = true;
  hold_deadline_ = std::chrono::steady_clock::now() + limit;
  return [this, generation = ++hold_generation_] { release_hold(generation); };
}

void CommandCoordinator::release_hold(std::uint64_t generation) {
  {
    std::lock_guard lock(mutex_);
    if (!hold_active_ || hold_generation_ != generation) return;
    hold_active_ = false;
  }
  condition_.notify_all();
}

void CommandCoordinator::run() {
  while (true) {
    Item item;
    {
      std::unique_lock lock(mutex_);
      while (!take_next(&item)) {
        if (stopping_) return;
        if (hold_active_)
          condition_.wait_until(lock, hold_deadline_);
        else
          condition_.wait(lock);
      }
    }

    const auto mark_accepted =
        [state = item.state](std::uint64_t accepted_sequence) {
          transition(state, CommandState::Accepted, {}, accepted_sequence);
        };

    try {
      if (item.context_action) {
        bool deferred = false;
        CommandContext context{mark_accepted,
                               {},
                               [&deferred] { deferred = true; },
                               [state = item.state] {
                                 transition(state, CommandState::Completed);
                               },
                               [state = item.state](std::string error) {
                                 transition(state, CommandState::Failed,
                                            std::move(error));
                               },
                               [this](std::chrono::milliseconds limit) {
                                 return hold_normal_commands(limit);
                               }};
        item.context_action(context);
        if (!deferred) transition(item.state, CommandState::Completed);
      } else {
        item.action();
        mark_accepted(0);
        transition(item.state, CommandState::Completed);
      }
    } catch (const std::exception& error) {
      transition(item.state, CommandState::Failed, error.what());
    } catch (...) {
      transition(item.state, CommandState::Failed,
                 "command failed with an unknown exception");
    }
  }
}

void CommandCoordinator::transition(
    const std::shared_ptr<CommandTicket::State>& state, CommandState result,
    std::string error, std::uint64_t accepted_sequence) {
  std::vector<CommandTicket::Observer> ready;
  CommandResult snapshot;
  {
    std::lock_guard lock(state->mutex);
    if (state->result.state == CommandState::Completed ||
        state->result.state == CommandState::Failed)
      return;
    if (result == CommandState::Accepted &&
        state->result.state != CommandState::Queued) {
      return;
    }
    if (result == CommandState::Accepted)
      state->result.accepted_sequence = accepted_sequence;
    state->result.state = result;
    state->result.error = std::move(error);
    snapshot = state->result;
    auto observer = state->observers.begin();
    while (observer != state->observers.end()) {
      const bool terminal =
          result == CommandState::Completed || result == CommandState::Failed;
      if (terminal || (observer->policy == CommandWaitPolicy::Accepted &&
                       result == CommandState::Accepted)) {
        ready.push_back(std::move(observer->callback));
        observer = state->observers.erase(observer);
      } else {
        ++observer;
      }
    }
    state->condition.notify_all();
  }
  for (const auto& callback : ready) callback(snapshot);
}

}  // namespace linuxcnc::server

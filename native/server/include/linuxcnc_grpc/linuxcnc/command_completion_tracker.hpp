#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace linuxcnc::server {

namespace detail {
constexpr bool nml_serial_after(std::int32_t lhs, std::int32_t rhs) {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(lhs) -
                                   static_cast<std::uint32_t>(rhs)) > 0;
}
}  // namespace detail

struct NmlErrorEvent {
  std::int32_t type = 0;
  std::string message;
  // Serial of the command LinuxCNC was handling when it raised this error,
  // the same value an ExecuteCommand response reports as its command
  // sequence. Zero when LinuxCNC did not tie the error to a command.
  std::uint32_t command_sequence = 0;
};

// The command-relevant part of one LinuxCNC status read.
struct NmlCommandEcho {
  enum class State { Other, Done, Error };
  std::int32_t serial = 0;
  State state = State::Other;
};

// Resolves written NML commands from status reads and from the errors
// LinuxCNC raised while handling them.
//
// LinuxCNC stamps an operator error with the serial of the command it was
// planning or issuing, and writes it before the status of that task cycle. A
// caller that reads a status, drains the error channel into error_read(), and
// then calls observe() has every error stamped with a serial before it
// resolves the command from a status that echoes that serial or a later one.
//
// A command is rejected when an error stamped with its serial was read, even
// if the status has already moved on to a later command, or when the status
// echoes its serial with an error. Its failure message is the text of its
// stamped errors. It completes when the status echoes it as done or has moved
// past it without either.
class NmlCommandCompletionTracker {
 public:
  using Completion = std::function<void()>;
  using Failure = std::function<void(std::string)>;

  static constexpr const char* kRejectedMessage =
      "LinuxCNC rejected the command";
  // A command's serial is known only after it was written, and LinuxCNC may
  // reject it before track() runs. Stamped errors are kept this long for a
  // command that is not tracked yet.
  static constexpr std::chrono::seconds kUnclaimedErrorRetention{1};
  static constexpr std::size_t kMaxUnclaimedErrors = 256;

  explicit NmlCommandCompletionTracker(std::chrono::milliseconds timeout);

  void track(std::int32_t serial, Completion completed, Failure failed);
  void channel_failed();
  // Records an error read from the error channel. command_serial is the
  // serial LinuxCNC stamped on it, or zero.
  void error_read(std::int32_t command_serial, std::string message);
  void observe(const NmlCommandEcho& echo);
  void tick();
  bool fresh() const;

 private:
  struct Pending {
    std::int32_t serial;
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t after_observation;
    Completion completed;
    Failure failed;
  };

  struct CommandError {
    std::int32_t serial;
    std::string message;
    std::chrono::steady_clock::time_point read_at;
  };

  enum class Resolution { Pending, Completed, Rejected, TimedOut, Unavailable };

  struct Ready {
    Pending pending;
    Resolution resolution;
    std::string reason;
  };

  Resolution resolve(const Pending& pending,
                     std::chrono::steady_clock::time_point now,
                     std::string* reason);
  void collect_ready(std::chrono::steady_clock::time_point now,
                     std::vector<Ready>* ready);
  bool is_pending(std::int32_t serial) const;
  void forget_unclaimed_errors(std::chrono::steady_clock::time_point now);
  static void finish(const Ready& ready);

  mutable std::mutex mutex_;
  NmlCommandEcho latest_;
  std::vector<Pending> pending_;
  std::deque<CommandError> command_errors_;
  std::chrono::steady_clock::time_point observed_at_{};
  bool have_snapshot_ = false;
  bool channel_healthy_ = false;
  std::uint64_t observation_sequence_ = 0;
  std::chrono::milliseconds timeout_{};
};

}  // namespace linuxcnc::server

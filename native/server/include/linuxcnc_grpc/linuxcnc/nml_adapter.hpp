#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "linuxcnc_grpc/command_coordinator.hpp"

namespace linuxcnc::server {

// LinuxCNC's EMC_TASK_PLAN_OPEN::file is a LINELEN-sized C string.
inline constexpr std::size_t kNmlProgramPathCapacity = 255;

namespace detail {
constexpr bool nml_serial_after(std::int32_t lhs, std::int32_t rhs) {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(lhs) -
                                   static_cast<std::uint32_t>(rhs)) > 0;
}
}  // namespace detail

enum class NmlCommandKind {
  SetTaskMode,
  SetTaskState,
  TaskPlanSynch,
  ResetInterpreter,
  ProgramOpen,
  ProgramClose,
  Run,
  Pause,
  Resume,
  Step,
  Reverse,
  Forward,
  StopProgram,
  AbortTask,
  SetOptionalStop,
  SetBlockDelete,
  Mdi,
  SetTrajMode,
  SetMaxVelocity,
  SetFeedRate,
  SetSpindleOverride,
  OverrideLimits,
  TeleopEnable,
  SetFeedOverrideEnable,
  SetSpindleOverrideEnable,
  SetFeedHoldEnable,
  SetAdaptiveFeedEnable,
  HomeJoint,
  UnhomeJoint,
  JogStop,
  JogContinuous,
  JogIncrement,
  SetMinPositionLimit,
  SetMaxPositionLimit,
  SpindleOn,
  SpindleIncrease,
  SpindleDecrease,
  SpindleOff,
  SpindleBrake,
  SetMist,
  SetFlood,
  LoadToolTable,
  SetTool,
  DeleteTool,
  SetDigitalOutput,
  SetAnalogOutput,
  SetDebugLevel,
  SendOperatorError,
  SendOperatorText,
  SendOperatorDisplay,
  SetRapidRate,
  SetJointAvailability,
  SetSpindleAvailability,
};

struct NmlPose {
  std::array<double, 9> values{};
};

struct NmlToolEntry {
  std::int32_t tool_no = 0;
  std::int32_t pocket_no = 0;
  NmlPose offset;
  NmlPose wear_offset;
  std::size_t offset_values = 0;
  std::size_t wear_offset_values = 0;
  double diameter = 0.0;
  double front_angle = 0.0;
  double back_angle = 0.0;
  std::int32_t orientation = 0;
  std::string comment;
  bool has_pocket_no = false;
  bool has_offset = false;
  bool has_wear_offset = false;
  bool has_diameter = false;
  bool has_front_angle = false;
  bool has_back_angle = false;
  bool has_orientation = false;
  bool has_comment = false;
};

struct NmlTaskStatus {
  std::int32_t mode = 0;
  std::int32_t state = 0;
  std::int32_t exec_state = 0;
  std::int32_t interp_state = 0;
  std::int32_t stop_state = 0;
  std::int32_t call_level = 0;
  std::int32_t motion_line = 0;
  std::int32_t current_line = 0;
  std::int32_t read_line = 0;
  bool optional_stop_state = false;
  bool block_delete_state = false;
  bool input_timeout = false;
  std::string file;
  std::string command;
  std::string ini_filename;
  NmlPose g5x_offset;
  std::int32_t g5x_index = 0;
  std::vector<NmlPose> g5x_offsets;
  std::vector<double> g5x_rotations;
  NmlPose g92_offset;
  NmlPose g28_position;
  NmlPose g30_position;
  double rotation_xy = 0.0;
  NmlPose tool_offset;
  std::vector<std::int32_t> active_g_codes;
  std::vector<std::int32_t> active_m_codes;
  std::vector<double> active_settings;
  std::int32_t program_units = 0;
  std::int32_t interpreter_error_code = 0;
  bool task_paused = false;
  double delay_left = 0.0;
  std::int32_t queued_mdi_commands = 0;
};

struct NmlJointStatus {
  std::int32_t joint_type = 0;
  double units = 0.0;
  double backlash = 0.0;
  double min_position_limit = 0.0;
  double max_position_limit = 0.0;
  double min_ferror = 0.0;
  double max_ferror = 0.0;
  double ferror_current = 0.0;
  double ferror_high_mark = 0.0;
  double output = 0.0;
  double input = 0.0;
  double velocity = 0.0;
  bool in_position = false;
  bool homing = false;
  bool homed = false;
  bool fault = false;
  bool enabled = false;
  bool min_soft_limit = false;
  bool max_soft_limit = false;
  bool min_hard_limit = false;
  bool max_hard_limit = false;
  bool override_limits = false;
  bool available = true;
};

struct NmlAxisStatus {
  double min_position_limit = 0.0;
  double max_position_limit = 0.0;
  double velocity = 0.0;
};

struct NmlSpindleStatus {
  double speed = 0.0;
  double feedback = 0.0;
  double override_scale = 0.0;
  double css_maximum = 0.0;
  double css_factor = 0.0;
  std::int32_t direction = 0;
  bool brake = false;
  std::int32_t increasing = 0;
  bool enabled = false;
  std::int32_t orient_state = 0;
  std::int32_t orient_fault = 0;
  bool spindle_override_enabled = false;
  bool homed = false;
  bool available = true;
};

struct NmlTrajectoryStatus {
  double linear_units = 0.0;
  double angular_units = 0.0;
  double cycle_time = 0.0;
  std::int32_t joints = 0;
  std::int32_t spindles = 0;
  std::vector<std::int32_t> available_axes;
  std::int32_t mode = 0;
  bool enabled = false;
  bool in_position = false;
  std::int32_t queue = 0;
  std::int32_t active_queue = 0;
  bool queue_full = false;
  std::int32_t id = 0;
  bool paused = false;
  bool single_stepping = false;
  double feed_rate_override = 0.0;
  double rapid_rate_override = 0.0;
  NmlPose position;
  NmlPose actual_position;
  double acceleration = 0.0;
  double max_velocity = 0.0;
  double max_acceleration = 0.0;
  NmlPose probed_position;
  bool probe_tripped = false;
  bool probing = false;
  double probe_val = 0.0;
  std::int32_t kinematics_type = 0;
  std::int32_t motion_type = 0;
  double distance_to_go = 0.0;
  NmlPose dtg;
  double current_velocity = 0.0;
  bool feed_override_enabled = false;
  bool adaptive_feed_enabled = false;
  bool feed_hold_enabled = false;
};

struct NmlMotionStatus {
  NmlTrajectoryStatus traj;
  std::vector<NmlJointStatus> joints;
  std::vector<NmlAxisStatus> axes;
  std::vector<NmlSpindleStatus> spindles;
  std::vector<std::int32_t> digital_input;
  std::vector<std::int32_t> digital_output;
  std::vector<double> analog_input;
  std::vector<double> analog_output;
};

struct NmlIoStatus {
  std::int32_t pocket_prepped = 0;
  std::int32_t tool_in_spindle = 0;
  std::int32_t tool_from_pocket = 0;
  bool mist = false;
  bool flood = false;
  bool estop = false;
};

struct NmlCommand {
  NmlCommandKind kind = NmlCommandKind::StopProgram;
  std::int32_t integer = 0;
  std::int32_t integer2 = 0;
  double number = 0.0;
  double number2 = 0.0;
  bool boolean = false;
  std::string text;
  std::string path;
  NmlPose pose;
  NmlPose wear_pose;
  NmlToolEntry tool;
  // Preparation is restricted to bounded handoff work on the serialized
  // command worker. Completion/failure callbacks finalize external state.
  std::function<void(NmlCommand&)> prepare;
  std::function<void()> on_completed;
  std::function<void()> on_failed;
};

struct NmlStatusSnapshot {
  std::int32_t echo_serial_number = 0;
  std::int32_t rcs_status = -1;
  std::int32_t task_mode = 0;
  std::int32_t task_state = 0;
  std::int32_t exec_state = 0;
  std::int32_t interp_state = 0;
  std::int32_t stop_state = 0;
  std::int32_t current_line = 0;
  std::int32_t motion_line = 0;
  std::int32_t read_line = 0;
  bool optional_stop = false;
  bool block_delete = false;
  std::string file;
  std::string command;
  std::string ini_filename;
  std::int32_t debug = 0;
  std::int32_t motion_type = 0;
  std::vector<double> actual_position;
  NmlTaskStatus task_stat;
  NmlMotionStatus motion_stat;
  NmlIoStatus io_stat;
  std::vector<NmlToolEntry> tool_table;
};

struct NmlPositionSnapshot {
  std::array<double, 9> commanded{};
  std::array<double, 9> tool_offset{};
  std::int32_t motion_type = 0;
};

struct NmlErrorEvent {
  std::int32_t type = 0;
  std::string message;
};

enum class NmlStatusPoll {
  Updated,
  Idle,
  Stale,
  Disconnected,
};

// Thin, transport-independent LinuxCNC NML boundary. The wire service maps
// protobuf commands/statuses to these value types; this class owns NML channel
// lifetime, serial command ordering, and completion observation.
class NmlAdapter {
 public:
  static constexpr std::int32_t kDigitalOutputLimit = 64;
  static constexpr std::int32_t kAnalogOutputLimit = 64;

  explicit NmlAdapter(
      std::string nml_file, std::size_t command_capacity = 128,
      std::chrono::milliseconds command_completion_timeout = {});
  ~NmlAdapter();

  NmlAdapter(const NmlAdapter&) = delete;
  NmlAdapter& operator=(const NmlAdapter&) = delete;

  NmlStatusPoll poll_status(NmlStatusSnapshot* snapshot);
  // Reads only fields required by high-rate position acquisition and avoids
  // materializing joints, axes, spindles, I/O, and the tool table.
  NmlStatusPoll poll_position(NmlPositionSnapshot* snapshot);
  std::optional<NmlErrorEvent> poll_error();
  CommandSubmission submit(NmlCommand command, std::stop_token stop_token = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server

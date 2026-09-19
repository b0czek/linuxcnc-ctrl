#include "linuxcnc_grpc/linuxcnc/command_validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace linuxcnc::server {
namespace {

NmlCommandValidation invalid(std::string message) {
  return {NmlCommandValidationCode::InvalidArgument, std::move(message)};
}

NmlCommandValidation unavailable() {
  return {NmlCommandValidationCode::StatusUnavailable,
          "LinuxCNC configuration status is unavailable"};
}

NmlCommandValidation finite(double value, const char* field) {
  if (!std::isfinite(value))
    return invalid(std::string(field) + " must be finite");
  return {};
}

NmlCommandValidation nonnegative(double value, const char* field) {
  auto result = finite(value, field);
  if (!result) return result;
  if (value < 0.0) return invalid(std::string(field) + " must not be negative");
  return {};
}

NmlCommandValidation configured_index(std::int32_t index, std::int32_t count,
                                      const char* field) {
  if (index < 0 || index >= count)
    return invalid(std::string(field) + " is out of configured range");
  return {};
}

bool has_axis(const NmlStatusSnapshot& status, std::int32_t index) {
  const auto& axes = status.motion_stat.traj.available_axes;
  return std::find(axes.begin(), axes.end(), index) != axes.end();
}

NmlCommandValidation validate_tool(const NmlToolEntry& tool) {
  if (tool.tool_no <= 0) return invalid("tool number must be positive");
  if (tool.has_pocket_no && (tool.pocket_no <= 0 || tool.pocket_no > 1000))
    return invalid("tool pocket must be between 1 and 1000");
  if (tool.offset_values > tool.offset.values.size() ||
      tool.wear_offset_values > tool.wear_offset.values.size())
    return invalid("tool offsets must contain at most 9 axes");
  if (tool.has_offset) {
    for (std::size_t index = 0; index < tool.offset_values; ++index) {
      auto result = finite(tool.offset.values[index], "tool offset");
      if (!result) return result;
    }
  }
  if (tool.has_wear_offset) {
    for (std::size_t index = 0; index < tool.wear_offset_values; ++index) {
      auto result = finite(tool.wear_offset.values[index], "tool wear offset");
      if (!result) return result;
    }
  }
  if (tool.has_diameter) {
    auto result = nonnegative(tool.diameter, "tool diameter");
    if (!result) return result;
  }
  if (tool.has_front_angle) {
    auto result = finite(tool.front_angle, "tool front angle");
    if (!result) return result;
  }
  if (tool.has_back_angle) {
    auto result = finite(tool.back_angle, "tool back angle");
    if (!result) return result;
  }
  if (tool.has_orientation && (tool.orientation < 0 || tool.orientation > 9))
    return invalid("tool orientation must be between 0 and 9");
  if (tool.has_comment && (tool.comment.size() >= 40 ||
                           tool.comment.find('\n') != std::string::npos ||
                           tool.comment.find('\r') != std::string::npos))
    return invalid("tool comment must be at most 39 bytes without newlines");
  return {};
}

}  // namespace

NmlCommandValidation validate_nml_command(
    const NmlCommand& command, const NmlStatusSnapshot* configuration) {
  switch (command.kind) {
    case NmlCommandKind::SetMaxVelocity:
      return nonnegative(command.number, "maximum velocity");
    case NmlCommandKind::SetFeedRate:
      return nonnegative(command.number, "feed override");
    case NmlCommandKind::SetRapidRate:
      return nonnegative(command.number, "rapid override");
    case NmlCommandKind::SetSpindleOverride: {
      auto result = nonnegative(command.number, "spindle override");
      if (!result) return result;
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.spindles,
                              "spindle index");
    }
    case NmlCommandKind::SetSpindleOverrideEnable:
    case NmlCommandKind::SpindleIncrease:
    case NmlCommandKind::SpindleDecrease:
    case NmlCommandKind::SpindleOff:
    case NmlCommandKind::SpindleBrake:
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.spindles,
                              "spindle index");
    case NmlCommandKind::SpindleOn: {
      auto result = finite(command.number, "spindle speed");
      if (!result) return result;
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.spindles,
                              "spindle index");
    }
    case NmlCommandKind::SetSpindleAvailability:
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.spindles,
                              "spindle index");
    case NmlCommandKind::SetJointAvailability:
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.joints,
                              "joint index");
    case NmlCommandKind::HomeJoint:
      if (!configuration) return unavailable();
      if (command.integer == -1) return {};
      return configured_index(command.integer,
                              configuration->motion_stat.traj.joints,
                              "joint index");
    case NmlCommandKind::UnhomeJoint:
      if (!configuration) return unavailable();
      if (command.integer == -2 || command.integer == -1) return {};
      return configured_index(command.integer,
                              configuration->motion_stat.traj.joints,
                              "joint index");
    case NmlCommandKind::JogStop:
    case NmlCommandKind::JogContinuous:
    case NmlCommandKind::JogIncrement: {
      if (command.kind != NmlCommandKind::JogStop) {
        auto result = finite(command.number, "jog speed");
        if (!result) return result;
      }
      if (command.kind == NmlCommandKind::JogIncrement) {
        auto result = finite(command.number2, "jog increment");
        if (!result) return result;
        if (command.number2 <= 0.0)
          return invalid("jog increment must be greater than zero");
      }
      if (!configuration) return unavailable();
      if (command.boolean)
        return configured_index(command.integer,
                                configuration->motion_stat.traj.joints,
                                "joint jog index");
      if (command.integer < 0 || !has_axis(*configuration, command.integer))
        return invalid("axis jog index is not configured");
      return {};
    }
    case NmlCommandKind::SetMinPositionLimit:
    case NmlCommandKind::SetMaxPositionLimit: {
      auto result = finite(command.number, "position limit");
      if (!result) return result;
      if (!configuration) return unavailable();
      return configured_index(command.integer,
                              configuration->motion_stat.traj.joints,
                              "joint index");
    }
    case NmlCommandKind::SetAnalogOutput:
      return finite(command.number, "analog output value");
    case NmlCommandKind::SetTool:
      return validate_tool(command.tool);
    case NmlCommandKind::DeleteTool:
      return command.integer > 0 ? NmlCommandValidation{}
                                 : invalid("tool number must be positive");
    default:
      return {};
  }
}

}  // namespace linuxcnc::server

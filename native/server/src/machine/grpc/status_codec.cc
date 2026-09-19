#include "machine/grpc/status_codec.hpp"

#include <algorithm>
#include <google/protobuf/util/message_differencer.h>

namespace linuxcnc::server::detail {

using namespace ::linuxcnc::v1;

namespace {

void fill_status(const NmlStatusSnapshot& source, LinuxCNCStat* target) {
  target->Clear();
  target->set_echo_serial_number(source.echo_serial_number);
  target->set_state(static_cast<RcsStatus>(source.rcs_status));
  target->set_debug(source.debug);
  auto fill_pose = [](const NmlPose& source, Position* target) {
    for (const auto value : source.values) target->add_values(value);
  };
  auto* task = target->mutable_task();
  const auto& task_source = source.task_stat;
  task->set_mode(static_cast<TaskMode>(task_source.mode));
  task->set_state(static_cast<TaskState>(task_source.state));
  task->set_exec_state(static_cast<ExecState>(task_source.exec_state));
  task->set_interp_state(static_cast<InterpState>(task_source.interp_state));
  task->set_stop_state(static_cast<StopState>(task_source.stop_state));
  task->set_call_level(task_source.call_level);
  task->set_motion_line(task_source.motion_line);
  task->set_current_line(task_source.current_line);
  task->set_read_line(task_source.read_line);
  task->set_optional_stop_state(task_source.optional_stop_state);
  task->set_block_delete_state(task_source.block_delete_state);
  task->set_input_timeout(task_source.input_timeout);
  task->set_file(task_source.file);
  task->set_command(task_source.command);
  task->set_ini_filename(task_source.ini_filename);
  fill_pose(task_source.g5x_offset, task->mutable_g5x_offset());
  task->set_g5x_index(task_source.g5x_index);
  for (const auto& pose : task_source.g5x_offsets)
    fill_pose(pose, task->add_g5x_offsets());
  for (const auto value : task_source.g5x_rotations)
    task->add_g5x_rotations(value);
  fill_pose(task_source.g92_offset, task->mutable_g92_offset());
  fill_pose(task_source.g28_position, task->mutable_g28_position());
  fill_pose(task_source.g30_position, task->mutable_g30_position());
  task->set_rotation_xy(task_source.rotation_xy);
  fill_pose(task_source.tool_offset, task->mutable_tool_offset());
  auto* g_codes = task->mutable_active_g_codes();
  if (task_source.active_g_codes.size() > 1)
    g_codes->set_motion_mode(task_source.active_g_codes[1]);
  if (task_source.active_g_codes.size() > 2)
    g_codes->set_g_mode0(task_source.active_g_codes[2]);
  if (task_source.active_g_codes.size() > 3)
    g_codes->set_plane(task_source.active_g_codes[3]);
  if (task_source.active_g_codes.size() > 4)
    g_codes->set_cutter_comp(task_source.active_g_codes[4]);
  if (task_source.active_g_codes.size() > 5)
    g_codes->set_units(task_source.active_g_codes[5]);
  if (task_source.active_g_codes.size() > 6)
    g_codes->set_distance_mode(task_source.active_g_codes[6]);
  if (task_source.active_g_codes.size() > 7)
    g_codes->set_feed_rate_mode(task_source.active_g_codes[7]);
  if (task_source.active_g_codes.size() > 8)
    g_codes->set_origin(task_source.active_g_codes[8]);
  if (task_source.active_g_codes.size() > 9)
    g_codes->set_tool_length_offset(task_source.active_g_codes[9]);
  if (task_source.active_g_codes.size() > 10)
    g_codes->set_retract_mode(task_source.active_g_codes[10]);
  if (task_source.active_g_codes.size() > 11)
    g_codes->set_path_control(task_source.active_g_codes[11]);
  if (task_source.active_g_codes.size() > 13)
    g_codes->set_spindle_speed_mode(task_source.active_g_codes[13]);
  if (task_source.active_g_codes.size() > 14)
    g_codes->set_ijk_distance_mode(task_source.active_g_codes[14]);
  if (task_source.active_g_codes.size() > 15)
    g_codes->set_lathe_diameter_mode(task_source.active_g_codes[15]);
  if (task_source.active_g_codes.size() > 16)
    g_codes->set_g92_applied(task_source.active_g_codes[16]);
  auto* m_codes = task->mutable_active_m_codes();
  if (task_source.active_m_codes.size() > 1)
    m_codes->set_stopping(task_source.active_m_codes[1]);
  if (task_source.active_m_codes.size() > 2)
    m_codes->set_spindle_control(task_source.active_m_codes[2]);
  if (task_source.active_m_codes.size() > 3)
    m_codes->set_tool_change(task_source.active_m_codes[3]);
  if (task_source.active_m_codes.size() > 4)
    m_codes->set_mist_coolant(task_source.active_m_codes[4]);
  if (task_source.active_m_codes.size() > 5)
    m_codes->set_flood_coolant(task_source.active_m_codes[5]);
  if (task_source.active_m_codes.size() > 6)
    m_codes->set_override_control(task_source.active_m_codes[6]);
  if (task_source.active_m_codes.size() > 7)
    m_codes->set_adaptive_feed_control(task_source.active_m_codes[7]);
  if (task_source.active_m_codes.size() > 8)
    m_codes->set_feed_hold_control(task_source.active_m_codes[8]);
  auto* settings = task->mutable_active_settings();
  if (task_source.active_settings.size() > 1)
    settings->set_feed_rate(task_source.active_settings[1]);
  if (task_source.active_settings.size() > 2)
    settings->set_speed(task_source.active_settings[2]);
  if (task_source.active_settings.size() > 3)
    settings->set_blend_tolerance(task_source.active_settings[3]);
  if (task_source.active_settings.size() > 4)
    settings->set_naive_cam_tolerance(task_source.active_settings[4]);
  task->set_program_units(static_cast<ProgramUnits>(task_source.program_units));
  task->set_interpreter_error_code(task_source.interpreter_error_code);
  task->set_task_paused(task_source.task_paused);
  task->set_delay_left(task_source.delay_left);
  task->set_queued_mdi_commands(task_source.queued_mdi_commands);

  const auto& motion_source = source.motion_stat;
  auto* trajectory = target->mutable_motion()->mutable_traj();
  const auto& source_traj = motion_source.traj;
  trajectory->set_linear_units(source_traj.linear_units);
  trajectory->set_angular_units(source_traj.angular_units);
  trajectory->set_cycle_time(source_traj.cycle_time);
  trajectory->set_joints(source_traj.joints);
  trajectory->set_spindles(source_traj.spindles);
  for (const auto axis : source_traj.available_axes)
    trajectory->add_available_axes(static_cast<AxisName>(axis + 1));
  trajectory->set_mode(static_cast<TrajMode>(source_traj.mode));
  trajectory->set_enabled(source_traj.enabled);
  trajectory->set_in_position(source_traj.in_position);
  trajectory->set_queue(source_traj.queue);
  trajectory->set_active_queue(source_traj.active_queue);
  trajectory->set_queue_full(source_traj.queue_full);
  trajectory->set_id(source_traj.id);
  trajectory->set_paused(source_traj.paused);
  trajectory->set_single_stepping(source_traj.single_stepping);
  trajectory->set_feed_rate_override(source_traj.feed_rate_override);
  trajectory->set_rapid_rate_override(source_traj.rapid_rate_override);
  fill_pose(source_traj.position, trajectory->mutable_position());
  fill_pose(source_traj.actual_position, trajectory->mutable_actual_position());
  trajectory->set_acceleration(source_traj.acceleration);
  trajectory->set_max_velocity(source_traj.max_velocity);
  trajectory->set_max_acceleration(source_traj.max_acceleration);
  fill_pose(source_traj.probed_position, trajectory->mutable_probed_position());
  trajectory->set_probe_tripped(source_traj.probe_tripped);
  trajectory->set_probing(source_traj.probing);
  trajectory->set_probe_val(source_traj.probe_val);
  trajectory->set_kinematics_type(
      static_cast<KinematicsType>(source_traj.kinematics_type));
  trajectory->set_motion_type(static_cast<MotionType>(source_traj.motion_type));
  trajectory->set_distance_to_go(source_traj.distance_to_go);
  fill_pose(source_traj.dtg, trajectory->mutable_dtg());
  trajectory->set_current_velocity(source_traj.current_velocity);
  trajectory->set_feed_override_enabled(source_traj.feed_override_enabled);
  trajectory->set_adaptive_feed_enabled(source_traj.adaptive_feed_enabled);
  trajectory->set_feed_hold_enabled(source_traj.feed_hold_enabled);
  auto* motion = target->mutable_motion();
  for (const auto& joint : motion_source.joints) {
    auto* value = motion->add_joint();
    value->set_joint_type(static_cast<JointType>(joint.joint_type));
    value->set_units(joint.units);
    value->set_backlash(joint.backlash);
    value->set_min_position_limit(joint.min_position_limit);
    value->set_max_position_limit(joint.max_position_limit);
    value->set_min_ferror(joint.min_ferror);
    value->set_max_ferror(joint.max_ferror);
    value->set_ferror_current(joint.ferror_current);
    value->set_ferror_high_mark(joint.ferror_high_mark);
    value->set_output(joint.output);
    value->set_input(joint.input);
    value->set_velocity(joint.velocity);
    value->set_in_position(joint.in_position);
    value->set_homing(joint.homing);
    value->set_homed(joint.homed);
    value->set_fault(joint.fault);
    value->set_enabled(joint.enabled);
    value->set_min_soft_limit(joint.min_soft_limit);
    value->set_max_soft_limit(joint.max_soft_limit);
    value->set_min_hard_limit(joint.min_hard_limit);
    value->set_max_hard_limit(joint.max_hard_limit);
    value->set_override_limits(joint.override_limits);
    value->set_available(joint.available);
  }
  for (const auto& axis : motion_source.axes) {
    auto* value = motion->add_axis();
    value->set_min_position_limit(axis.min_position_limit);
    value->set_max_position_limit(axis.max_position_limit);
    value->set_velocity(axis.velocity);
  }
  for (const auto& spindle : motion_source.spindles) {
    auto* value = motion->add_spindle();
    value->set_speed(spindle.speed);
    value->set_feedback(spindle.feedback);
    value->set_override(spindle.override_scale);
    value->set_css_maximum(spindle.css_maximum);
    value->set_css_factor(spindle.css_factor);
    value->set_direction(spindle.direction);
    value->set_brake(spindle.brake);
    value->set_increasing(spindle.increasing);
    value->set_enabled(spindle.enabled);
    value->set_orient_state(static_cast<OrientState>(spindle.orient_state));
    value->set_orient_fault(spindle.orient_fault);
    value->set_spindle_override_enabled(spindle.spindle_override_enabled);
    value->set_homed(spindle.homed);
    value->set_available(spindle.available);
  }
  for (const auto value : motion_source.digital_input)
    motion->add_digital_input(value);
  for (const auto value : motion_source.digital_output)
    motion->add_digital_output(value);
  for (const auto value : motion_source.analog_input)
    motion->add_analog_input(value);
  for (const auto value : motion_source.analog_output)
    motion->add_analog_output(value);
  const auto& io_source = source.io_stat;
  auto* io = target->mutable_io();
  io->mutable_tool()->set_pocket_prepped(io_source.pocket_prepped);
  io->mutable_tool()->set_tool_in_spindle(io_source.tool_in_spindle);
  io->mutable_tool()->set_tool_from_pocket(io_source.tool_from_pocket);
  io->mutable_coolant()->set_mist(io_source.mist);
  io->mutable_coolant()->set_flood(io_source.flood);
  io->set_estop(io_source.estop);
  for (const auto& tool : source.tool_table) {
    auto* value = target->add_tool_table();
    value->set_tool_no(tool.tool_no);
    value->set_pocket_no(tool.pocket_no);
    fill_pose(tool.offset, value->mutable_offset());
    fill_pose(tool.wear_offset, value->mutable_wear_offset());
    value->set_diameter(tool.diameter);
    value->set_front_angle(tool.front_angle);
    value->set_back_angle(tool.back_angle);
    value->set_orientation(tool.orientation);
    value->set_comment(tool.comment);
  }
}

template <typename Message>
bool message_equal(const Message& left, const Message& right) {
  return google::protobuf::util::MessageDifferencer::Equals(left, right);
}

template <typename Repeated>
bool repeated_equal(const Repeated& left, const Repeated& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

template <typename Repeated>
bool repeated_message_equal(const Repeated& left, const Repeated& right) {
  if (left.size() != right.size()) return false;
  for (int index = 0; index < left.size(); ++index) {
    if (!message_equal(left.Get(index), right.Get(index))) return false;
  }
  return true;
}

bool copy_task_delta(const TaskStat& previous, const TaskStat& current,
                     TaskStatDelta* target) {
  bool changed = false;
#define COPY_TASK_SCALAR(name)             \
  if (previous.name() != current.name()) { \
    target->set_##name(current.name());    \
    changed = true;                        \
  }
  COPY_TASK_SCALAR(mode)
  COPY_TASK_SCALAR(state)
  COPY_TASK_SCALAR(exec_state)
  COPY_TASK_SCALAR(interp_state)
  COPY_TASK_SCALAR(stop_state)
  COPY_TASK_SCALAR(call_level)
  COPY_TASK_SCALAR(motion_line)
  COPY_TASK_SCALAR(current_line)
  COPY_TASK_SCALAR(read_line)
  COPY_TASK_SCALAR(optional_stop_state)
  COPY_TASK_SCALAR(block_delete_state)
  COPY_TASK_SCALAR(input_timeout)
  COPY_TASK_SCALAR(file)
  COPY_TASK_SCALAR(command)
  COPY_TASK_SCALAR(ini_filename)
  COPY_TASK_SCALAR(g5x_index)
  COPY_TASK_SCALAR(rotation_xy)
  COPY_TASK_SCALAR(program_units)
  COPY_TASK_SCALAR(interpreter_error_code)
  COPY_TASK_SCALAR(task_paused)
  COPY_TASK_SCALAR(delay_left)
  COPY_TASK_SCALAR(queued_mdi_commands)
#undef COPY_TASK_SCALAR

#define COPY_TASK_MESSAGE(name)                         \
  if (!message_equal(previous.name(), current.name())) { \
    *target->mutable_##name() = current.name();          \
    changed = true;                                      \
  }
  COPY_TASK_MESSAGE(g5x_offset)
  COPY_TASK_MESSAGE(g92_offset)
  COPY_TASK_MESSAGE(g28_position)
  COPY_TASK_MESSAGE(g30_position)
  COPY_TASK_MESSAGE(tool_offset)
  COPY_TASK_MESSAGE(active_g_codes)
  COPY_TASK_MESSAGE(active_m_codes)
  COPY_TASK_MESSAGE(active_settings)
#undef COPY_TASK_MESSAGE

  if (!repeated_message_equal(previous.g5x_offsets(), current.g5x_offsets())) {
    for (const auto& value : current.g5x_offsets())
      *target->add_g5x_offsets() = value;
    target->set_replace_g5x_offsets(true);
    changed = true;
  }
  if (!repeated_equal(previous.g5x_rotations(), current.g5x_rotations())) {
    for (const auto value : current.g5x_rotations())
      target->add_g5x_rotations(value);
    target->set_replace_g5x_rotations(true);
    changed = true;
  }
  return changed;
}

bool copy_trajectory_delta(const TrajectoryStat& previous,
                           const TrajectoryStat& current,
                           TrajectoryStatDelta* target) {
  bool changed = false;
#define COPY_TRAJ_SCALAR(name)             \
  if (previous.name() != current.name()) { \
    target->set_##name(current.name());    \
    changed = true;                        \
  }
  COPY_TRAJ_SCALAR(linear_units)
  COPY_TRAJ_SCALAR(angular_units)
  COPY_TRAJ_SCALAR(cycle_time)
  COPY_TRAJ_SCALAR(joints)
  COPY_TRAJ_SCALAR(spindles)
  COPY_TRAJ_SCALAR(mode)
  COPY_TRAJ_SCALAR(enabled)
  COPY_TRAJ_SCALAR(in_position)
  COPY_TRAJ_SCALAR(queue)
  COPY_TRAJ_SCALAR(active_queue)
  COPY_TRAJ_SCALAR(queue_full)
  COPY_TRAJ_SCALAR(id)
  COPY_TRAJ_SCALAR(paused)
  COPY_TRAJ_SCALAR(single_stepping)
  COPY_TRAJ_SCALAR(feed_rate_override)
  COPY_TRAJ_SCALAR(rapid_rate_override)
  COPY_TRAJ_SCALAR(acceleration)
  COPY_TRAJ_SCALAR(max_velocity)
  COPY_TRAJ_SCALAR(max_acceleration)
  COPY_TRAJ_SCALAR(probe_tripped)
  COPY_TRAJ_SCALAR(probing)
  COPY_TRAJ_SCALAR(probe_val)
  COPY_TRAJ_SCALAR(kinematics_type)
  COPY_TRAJ_SCALAR(motion_type)
  COPY_TRAJ_SCALAR(distance_to_go)
  COPY_TRAJ_SCALAR(current_velocity)
  COPY_TRAJ_SCALAR(feed_override_enabled)
  COPY_TRAJ_SCALAR(adaptive_feed_enabled)
  COPY_TRAJ_SCALAR(feed_hold_enabled)
#undef COPY_TRAJ_SCALAR

#define COPY_TRAJ_MESSAGE(name)                         \
  if (!message_equal(previous.name(), current.name())) { \
    *target->mutable_##name() = current.name();          \
    changed = true;                                      \
  }
  COPY_TRAJ_MESSAGE(position)
  COPY_TRAJ_MESSAGE(actual_position)
  COPY_TRAJ_MESSAGE(probed_position)
  COPY_TRAJ_MESSAGE(dtg)
#undef COPY_TRAJ_MESSAGE

  if (!repeated_equal(previous.available_axes(), current.available_axes())) {
    for (const auto value : current.available_axes())
      target->add_available_axes(static_cast<AxisName>(value));
    target->set_replace_available_axes(true);
    changed = true;
  }
  return changed;
}

}  // namespace

EncodedStatus encode_status(const NmlStatusSnapshot& source) {
  EncodedStatus encoded;
  fill_status(source, &encoded.message);
  return encoded;
}

std::optional<LinuxCNCStatDelta> make_status_delta(
    const EncodedStatus& previous, const EncodedStatus& current,
    std::uint64_t sequence) {
  const auto& previous_wire = previous.message;
  const auto& current_wire = current.message;
  LinuxCNCStatDelta delta;
  delta.set_sequence(static_cast<std::int64_t>(sequence));
  bool changed = false;
  if (previous_wire.state() != current_wire.state()) {
    delta.set_state(current_wire.state());
    changed = true;
  }
  if (previous_wire.echo_serial_number() != current_wire.echo_serial_number()) {
    delta.set_echo_serial_number(current_wire.echo_serial_number());
    changed = true;
  }
  if (previous_wire.debug() != current_wire.debug()) {
    delta.set_debug(current_wire.debug());
    changed = true;
  }
  if (copy_task_delta(previous_wire.task(), current_wire.task(),
                      delta.mutable_task())) {
    changed = true;
  } else {
    delta.clear_task();
  }
  const auto& previous_motion = previous_wire.motion();
  const auto& current_motion = current_wire.motion();
  auto* motion = delta.mutable_motion();
  bool motion_changed = false;
  if (copy_trajectory_delta(previous_motion.traj(), current_motion.traj(),
                            motion->mutable_traj())) {
    motion_changed = true;
  } else {
    motion->clear_traj();
  }
  const auto joint_count =
      std::max(previous_motion.joint_size(), current_motion.joint_size());
  for (int index = 0; index < joint_count; ++index) {
    if (index >= previous_motion.joint_size() ||
        index >= current_motion.joint_size() ||
        !message_equal(previous_motion.joint(index),
                       current_motion.joint(index))) {
      auto* item = motion->add_joint();
      item->set_index(static_cast<std::uint32_t>(index));
      if (index < current_motion.joint_size())
        *item->mutable_value() = current_motion.joint(index);
      motion_changed = true;
    }
  }
  const auto axis_count =
      std::max(previous_motion.axis_size(), current_motion.axis_size());
  for (int index = 0; index < axis_count; ++index) {
    if (index >= previous_motion.axis_size() ||
        index >= current_motion.axis_size() ||
        !message_equal(previous_motion.axis(index), current_motion.axis(index))) {
      auto* item = motion->add_axis();
      item->set_index(static_cast<std::uint32_t>(index));
      if (index < current_motion.axis_size())
        *item->mutable_value() = current_motion.axis(index);
      motion_changed = true;
    }
  }
  const auto spindle_count =
      std::max(previous_motion.spindle_size(), current_motion.spindle_size());
  for (int index = 0; index < spindle_count; ++index) {
    if (index >= previous_motion.spindle_size() ||
        index >= current_motion.spindle_size() ||
        !message_equal(previous_motion.spindle(index),
                       current_motion.spindle(index))) {
      auto* item = motion->add_spindle();
      item->set_index(static_cast<std::uint32_t>(index));
      if (index < current_motion.spindle_size())
        *item->mutable_value() = current_motion.spindle(index);
      motion_changed = true;
    }
  }
  if (!repeated_equal(previous_motion.digital_input(),
                      current_motion.digital_input())) {
    motion->set_replace_digital_input(true);
    for (const auto value : current_motion.digital_input())
      motion->add_digital_input(value);
    motion_changed = true;
  }
  if (!repeated_equal(previous_motion.digital_output(),
                      current_motion.digital_output())) {
    motion->set_replace_digital_output(true);
    for (const auto value : current_motion.digital_output())
      motion->add_digital_output(value);
    motion_changed = true;
  }
  if (!repeated_equal(previous_motion.analog_input(),
                      current_motion.analog_input())) {
    motion->set_replace_analog_input(true);
    for (const auto value : current_motion.analog_input())
      motion->add_analog_input(value);
    motion_changed = true;
  }
  if (!repeated_equal(previous_motion.analog_output(),
                      current_motion.analog_output())) {
    motion->set_replace_analog_output(true);
    for (const auto value : current_motion.analog_output())
      motion->add_analog_output(value);
    motion_changed = true;
  }
  if (motion_changed) {
    changed = true;
  } else {
    delta.clear_motion();
  }

  const auto& previous_io = previous_wire.io();
  const auto& current_io = current_wire.io();
  auto* io = delta.mutable_io();
  bool io_changed = false;
  if (!message_equal(previous_io.tool(), current_io.tool())) {
    *io->mutable_tool() = current_io.tool();
    io_changed = true;
  }
  if (!message_equal(previous_io.coolant(), current_io.coolant())) {
    *io->mutable_coolant() = current_io.coolant();
    io_changed = true;
  }
  if (previous_io.estop() != current_io.estop()) {
    io->set_estop(current_io.estop());
    io_changed = true;
  }
  if (io_changed) {
    changed = true;
  } else {
    delta.clear_io();
  }

  if (!repeated_message_equal(previous_wire.tool_table(),
                              current_wire.tool_table())) {
    auto* table = delta.mutable_tool_table();
    for (const auto& tool : current_wire.tool_table())
      *table->add_tools() = tool;
    changed = true;
  }
  if (!changed) return std::nullopt;
  return delta;
}

}  // namespace linuxcnc::server::detail

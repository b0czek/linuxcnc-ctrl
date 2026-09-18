#include <cassert>
#include <cstdint>

#include "machine/grpc/status_codec.hpp"

using namespace linuxcnc::server;
using namespace linuxcnc::server::detail;

namespace {

NmlStatusSnapshot status_sample() {
  NmlStatusSnapshot sample;
  sample.echo_serial_number = 17;
  sample.rcs_status = linuxcnc::v1::RCS_STATUS_EXEC;
  sample.debug = 3;
  sample.task_stat.mode = linuxcnc::v1::TASK_MODE_AUTO;
  sample.task_stat.file = "program.ngc";
  sample.task_stat.optional_stop_state = false;
  sample.task_stat.motion_line = 42;
  sample.task_stat.g5x_offsets.resize(2);
  sample.task_stat.g5x_offsets[0].values[0] = 1.0;
  sample.task_stat.g5x_offsets[1].values[0] = 2.0;
  sample.task_stat.g5x_rotations = {10.0, 20.0};
  sample.motion_stat.traj.joints = 2;
  sample.motion_stat.traj.actual_position.values[0] = 12.5;
  sample.motion_stat.joints.resize(2);
  sample.motion_stat.joints[0].homed = true;
  sample.motion_stat.joints[0].available = false;
  sample.motion_stat.joints[1].homed = true;
  sample.motion_stat.axes.resize(2);
  sample.motion_stat.axes[0].velocity = 2.5;
  sample.motion_stat.axes[1].velocity = 3.5;
  sample.motion_stat.spindles.resize(2);
  sample.motion_stat.spindles[0].speed = 1000.0;
  sample.motion_stat.spindles[1].speed = 2000.0;
  sample.motion_stat.digital_input = {0, 1};
  sample.motion_stat.digital_output = {1, 0};
  sample.motion_stat.analog_input = {1.5, 2.5};
  sample.motion_stat.analog_output = {3.5, 4.5};
  sample.io_stat.tool_in_spindle = 7;
  sample.tool_table.resize(2);
  sample.tool_table[0].tool_no = 7;
  sample.tool_table[0].diameter = 4.0;
  sample.tool_table[1].tool_no = 8;
  sample.tool_table[1].diameter = 5.0;
  return sample;
}

void snapshot_encoding_is_stable() {
  auto source = status_sample();
  const auto encoded = encode_status(source);
  assert(encoded.message.echo_serial_number() == 17);
  assert(encoded.message.state() == linuxcnc::v1::RCS_STATUS_EXEC);
  assert(encoded.message.task().file() == "program.ngc");
  assert(encoded.message.motion().traj().actual_position().values(0) == 12.5);
  assert(encoded.message.motion().joint(0).homed());
  assert(!encoded.message.motion().joint(0).available());
  assert(encoded.message.io().tool().tool_in_spindle() == 7);
  assert(encoded.message.tool_table(0).diameter() == 4.0);

  source.task_stat.file = "changed.ngc";
  assert(encoded.message.task().file() == "program.ngc");
  assert(encoded.serialized == encoded.message.SerializeAsString());
  assert(!make_status_delta(encoded, encoded, 2));
}

void delta_preserves_wire_changes() {
  const auto previous = encode_status(status_sample());
  auto changed = status_sample();
  changed.echo_serial_number = 18;
  changed.task_stat.optional_stop_state = true;
  changed.motion_stat.traj.actual_position.values[0] = 15.0;
  changed.motion_stat.joints[0].homed = false;
  changed.motion_stat.digital_input = {1, 0};
  changed.io_stat.tool_in_spindle = 8;
  changed.tool_table[0].diameter = 5.0;
  const auto current = encode_status(changed);

  const auto delta = make_status_delta(previous, current, 23);
  assert(delta);
  assert(delta->sequence() == 23);
  assert(delta->echo_serial_number() == 18);
  assert(delta->task().optional_stop_state());
  assert(delta->motion().traj().actual_position().values(0) == 15.0);
  assert(delta->motion().joint_size() == 1);
  assert(!delta->motion().joint(0).value().homed());
  assert(delta->motion().replace_digital_input());
  assert(delta->motion().digital_input(0) == 1);
  assert(delta->io().tool().tool_in_spindle() == 8);
  assert(delta->tool_table().replace_all());
  assert(delta->tool_table().replaced(0).diameter() == 5.0);
}

void skipped_sample_delta_reconstructs_resets_and_shrinking_collections() {
  const auto previous = encode_status(status_sample());

  // This observation is intentionally skipped. StatusReactor compares its
  // last delivered sample directly with the newest retained sample.
  auto skipped_source = status_sample();
  skipped_source.debug = 99;
  skipped_source.motion_stat.digital_input = {1, 1, 1};
  const auto skipped = encode_status(skipped_source);
  assert(make_status_delta(previous, skipped, 22));

  auto current_source = status_sample();
  current_source.echo_serial_number = 0;
  current_source.rcs_status = linuxcnc::v1::RCS_STATUS_UNSPECIFIED;
  current_source.debug = 0;
  current_source.task_stat.mode = linuxcnc::v1::TASK_MODE_UNSPECIFIED;
  current_source.task_stat.file.clear();
  current_source.task_stat.motion_line = 0;
  current_source.task_stat.g5x_offsets.clear();
  current_source.task_stat.g5x_rotations.clear();
  current_source.motion_stat.joints.resize(1);
  current_source.motion_stat.joints[0] = {};
  current_source.motion_stat.axes.clear();
  current_source.motion_stat.spindles.resize(1);
  current_source.motion_stat.spindles[0] = {};
  current_source.motion_stat.digital_input.clear();
  current_source.motion_stat.digital_output.clear();
  current_source.motion_stat.analog_input.clear();
  current_source.motion_stat.analog_output.clear();
  current_source.io_stat.tool_in_spindle = 0;
  current_source.tool_table.clear();
  const auto current = encode_status(current_source);

  const auto delta = make_status_delta(previous, current, 23);
  assert(delta);
  assert(delta->sequence() == 23);
  assert(delta->has_echo_serial_number() && delta->echo_serial_number() == 0);
  assert(delta->has_state() &&
         delta->state() == linuxcnc::v1::RCS_STATUS_UNSPECIFIED);
  assert(delta->has_debug() && delta->debug() == 0);
  assert(delta->task().has_mode() &&
         delta->task().mode() == linuxcnc::v1::TASK_MODE_UNSPECIFIED);
  assert(delta->task().has_file() && delta->task().file().empty());
  assert(delta->task().has_motion_line() && delta->task().motion_line() == 0);
  assert(delta->task().replace_g5x_offsets());
  assert(delta->task().g5x_offsets().empty());
  assert(delta->task().replace_g5x_rotations());
  assert(delta->task().g5x_rotations().empty());

  const auto& motion = delta->motion();
  assert(motion.joint_size() == 2);
  assert(motion.joint(0).index() == 0 && motion.joint(0).has_value());
  assert(!motion.joint(0).value().homed());
  assert(motion.joint(1).index() == 1 && !motion.joint(1).has_value());
  assert(motion.axis_size() == 2);
  assert(!motion.axis(0).has_value() && !motion.axis(1).has_value());
  assert(motion.spindle_size() == 2);
  assert(motion.spindle(0).has_value() &&
         motion.spindle(0).value().speed() == 0.0);
  assert(!motion.spindle(1).has_value());
  assert(motion.replace_digital_input() && motion.digital_input().empty());
  assert(motion.replace_digital_output() && motion.digital_output().empty());
  assert(motion.replace_analog_input() && motion.analog_input().empty());
  assert(motion.replace_analog_output() && motion.analog_output().empty());
  assert(delta->io().tool().tool_in_spindle() == 0);
  assert(delta->tool_table().replace_all());
  assert(delta->tool_table().replaced().empty());
}

}  // namespace

int main() {
  snapshot_encoding_is_stable();
  delta_preserves_wire_changes();
  skipped_sample_delta_reconstructs_resets_and_shrinking_collections();
  return 0;
}

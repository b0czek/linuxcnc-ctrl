#include "recording_canon.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace recording = linuxcnc::recording;

// LinuxCNC requires these global names from every canon implementation.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
int _task = 0;
// NOLINTNEXTLINE(bugprone-reserved-identifier)
char _parameter_file_name[PARAMETER_FILE_NAME_LENGTH]{};
EmcPose tool_offset{};
USER_DEFINED_FUNCTION_TYPE USER_DEFINED_FUNCTION[USER_DEFINED_FUNCTION_NUM]{};

namespace linuxcnc::recording {

struct Session::Impl {
  std::vector<Event> events;
  EmcPose position{};
  EmcPose probe_position{};
  CANON_UNITS units = CANON_UNITS_INCHES;
  int axis_mask = 0x7;
  std::vector<CANON_TOOL_TABLE> tools;
  std::size_t max_events = 0;
  std::size_t max_bytes = 0;
  std::size_t recorded_bytes = 0;
  std::size_t max_control_points = 0;
  bool exceeded = false;
  CANON_PLANE plane = CANON_PLANE::XY;
  CANON_MOTION_MODE motion_mode = CANON_CONTINUOUS;
  double motion_tolerance = 0.1;
  double naivecam_tolerance = 0.1;
  double feed_rate = 1.0;
  double traverse_rate = 0.0;
  double spindle_speed[EMCMOT_MAX_SPINDLES]{};
  CANON_DIRECTION spindle_direction[EMCMOT_MAX_SPINDLES]{};
  int selected_tool = 0;
  int tool_slot = 0;
  bool block_delete = false;
  bool optional_stop = false;
  bool flood = false;
  bool mist = false;
};

namespace {

thread_local Session::Impl* active_session = nullptr;

Session::Impl& active() {
  assert(active_session && "recording canon call without an active Session");
  return *active_session;
}

template <typename T>
std::size_t event_size(const T&) {
  return sizeof(T);
}

std::size_t event_size(const NurbsG5Feed& event) {
  return sizeof(event) +
         event.control_points.size() * sizeof(NURBS_CONTROL_POINT);
}

std::size_t event_size(const NurbsG6Feed& event) {
  return sizeof(event) +
         event.control_points.size() * sizeof(NURBS_G6_CONTROL_POINT);
}

template <typename T>
void record(T&& event) {
  auto& state = active();
  const auto bytes = event_size(event);
  if (state.events.size() >= state.max_events ||
      bytes >
          state.max_bytes - std::min(state.recorded_bytes, state.max_bytes)) {
    state.exceeded = true;
    return;
  }
  state.recorded_bytes += bytes;
  state.events.emplace_back(std::forward<T>(event));
}

EmcPose pose(double x, double y, double z, double a, double b, double c,
             double u, double v, double w) {
  EmcPose result{};
  result.tran.x = x;
  result.tran.y = y;
  result.tran.z = z;
  result.a = a;
  result.b = b;
  result.c = c;
  result.u = u;
  result.v = v;
  result.w = w;
  return result;
}

void scale_pose(EmcPose& value, double scale) {
  value.tran.x *= scale;
  value.tran.y *= scale;
  value.tran.z *= scale;
  value.u *= scale;
  value.v *= scale;
  value.w *= scale;
}

double millimetres_per_unit(CANON_UNITS units) {
  switch (units) {
    case CANON_UNITS_INCHES:
      return 25.4;
    case CANON_UNITS_MM:
      return 1.0;
    case CANON_UNITS_CM:
      return 10.0;
  }
  return 1.0;
}

void set_plane_end(EmcPose& position, CANON_PLANE plane, double first,
                   double second, double axis) {
  switch (plane) {
    case CANON_PLANE::XY:
      position.tran.x = first;
      position.tran.y = second;
      position.tran.z = axis;
      break;
    case CANON_PLANE::YZ:
      position.tran.y = first;
      position.tran.z = second;
      position.tran.x = axis;
      break;
    case CANON_PLANE::XZ:
      position.tran.z = first;
      position.tran.x = second;
      position.tran.y = axis;
      break;
    case CANON_PLANE::UV:
      position.u = first;
      position.v = second;
      break;
    case CANON_PLANE::VW:
      position.v = first;
      position.w = second;
      break;
    case CANON_PLANE::UW:
      position.u = first;
      position.w = second;
      break;
  }
}

double orthogonal_axis(const EmcPose& position, CANON_PLANE plane) {
  switch (plane) {
    case CANON_PLANE::XY:
      return position.tran.z;
    case CANON_PLANE::YZ:
      return position.tran.x;
    case CANON_PLANE::XZ:
      return position.tran.y;
    default:
      return 0.0;
  }
}

bool valid_spindle(int spindle) {
  return spindle >= 0 && spindle < EMCMOT_MAX_SPINDLES;
}

}  // namespace

Session::Session(SessionConfig config) : impl(std::make_unique<Impl>()) {
  assert(!active_session && "recording canon Sessions may not overlap");
  active_session = impl.get();
  impl->units = config.units;
  impl->axis_mask = config.axis_mask;
  impl->tools = std::move(config.tools);
  impl->max_events = config.max_events;
  impl->max_bytes = config.max_bytes;
  impl->max_control_points = config.max_control_points;
  tool_offset = {};
}

bool Session::limit_exceeded() const { return impl->exceeded; }

Session::~Session() {
  assert(active_session == impl.get());
  active_session = nullptr;
}

std::vector<Event> Session::take_events() {
  std::vector<Event> result;
  result.swap(impl->events);
  impl->recorded_bytes = 0;
  return result;
}

}  // namespace linuxcnc::recording

void INIT_CANON() {}

void STRAIGHT_TRAVERSE(int line_number, double x, double y, double z, double a,
                       double b, double c, double u, double v, double w) {
  auto& state = recording::active();
  state.position = recording::pose(x, y, z, a, b, c, u, v, w);
  recording::record(recording::StraightTraverse{line_number, state.position});
}

void STRAIGHT_FEED(int line_number, double x, double y, double z, double a,
                   double b, double c, double u, double v, double w) {
  auto& state = recording::active();
  state.position = recording::pose(x, y, z, a, b, c, u, v, w);
  recording::record(recording::StraightFeed{line_number, state.position});
}

void ARC_FEED(int line_number, double first_end, double second_end,
              double first_axis, double second_axis, int rotation,
              double axis_end_point, double a, double b, double c, double u,
              double v, double w) {
  auto& state = recording::active();
  recording::record(recording::ArcFeed{
      line_number, first_end, second_end, first_axis, second_axis, rotation,
      axis_end_point, a, b, c, u, v, w, state.plane});
  recording::set_plane_end(state.position, state.plane, first_end, second_end,
                           axis_end_point);
  state.position.a = a;
  state.position.b = b;
  state.position.c = c;
  state.position.u = u;
  state.position.v = v;
  state.position.w = w;
}

void STRAIGHT_PROBE(int line_number, double x, double y, double z, double a,
                    double b, double c, double u, double v, double w,
                    unsigned char probe_type) {
  auto& state = recording::active();
  state.probe_position = recording::pose(x, y, z, a, b, c, u, v, w);
  state.position = state.probe_position;
  recording::record(
      recording::StraightProbe{line_number, state.position, probe_type});
}

void RIGID_TAP(int line_number, double x, double y, double z, double scale) {
  auto& state = recording::active();
  recording::record(recording::RigidTap{line_number, x, y, z, scale});
  state.position.tran.x = x;
  state.position.tran.y = y;
}

void DWELL(double seconds) { recording::record(recording::Dwell{seconds}); }

void NURBS_G5_FEED(int line_number,
                   const std::vector<NURBS_CONTROL_POINT>& control_points,
                   unsigned int order, CANON_PLANE plane) {
  auto& state = recording::active();
  if (control_points.size() > state.max_control_points) {
    state.exceeded = true;
    return;
  }
  recording::record(
      recording::NurbsG5Feed{line_number, control_points, order, plane});
  if (!control_points.empty()) {
    const auto& last = control_points.back();
    recording::set_plane_end(state.position, plane, last.NURBS_X, last.NURBS_Y,
                             recording::orthogonal_axis(state.position, plane));
  }
}

void NURBS_G6_FEED(int line_number,
                   const std::vector<NURBS_G6_CONTROL_POINT>& control_points,
                   unsigned int order, double feed_rate, int l_option,
                   CANON_PLANE plane) {
  auto& state = recording::active();
  if (control_points.size() > state.max_control_points) {
    state.exceeded = true;
    return;
  }
  recording::record(recording::NurbsG6Feed{line_number, control_points, order,
                                           feed_rate, l_option, plane});
  if (control_points.size() > order) {
    const auto& last = control_points.back();
    recording::set_plane_end(state.position, plane, last.NURBS_X, last.NURBS_Y,
                             recording::orthogonal_axis(state.position, plane));
  }
}

void USE_LENGTH_UNITS(CANON_UNITS units) {
  auto& state = recording::active();
  if (state.units != units) {
    const double scale = recording::millimetres_per_unit(state.units) /
                         recording::millimetres_per_unit(units);
    recording::scale_pose(state.position, scale);
    recording::scale_pose(state.probe_position, scale);
    recording::scale_pose(tool_offset, scale);
    state.units = units;
  }
  recording::record(recording::LengthUnits{units});
}

void SELECT_PLANE(CANON_PLANE plane) {
  auto& state = recording::active();
  state.plane = plane;
  recording::record(recording::Plane{plane});
}

void SET_G5X_OFFSET(int index, double x, double y, double z, double a, double b,
                    double c, double u, double v, double w) {
  recording::record(
      recording::G5xOffset{index, recording::pose(x, y, z, a, b, c, u, v, w)});
}

void SET_G92_OFFSET(double x, double y, double z, double a, double b, double c,
                    double u, double v, double w) {
  recording::record(
      recording::G92Offset{recording::pose(x, y, z, a, b, c, u, v, w)});
}

void SET_XY_ROTATION(double degrees) {
  recording::record(recording::XyRotation{degrees});
}

void HOME_CYCLE() {}
void HOME_CYCLE_JOINT(int) {}

void CANON_UPDATE_END_POINT(double x, double y, double z, double a, double b,
                            double c, double u, double v, double w) {
  recording::active().position = recording::pose(x, y, z, a, b, c, u, v, w);
}

void SET_FEED_RATE(double rate) {
  auto& state = recording::active();
  state.feed_rate = rate;
  recording::record(recording::FeedRate{rate});
}

void SET_TRAVERSE_RATE(double rate) {
  recording::active().traverse_rate = rate;
}
void SET_FEED_REFERENCE(CANON_FEED_REFERENCE) {}
void SET_FEED_MODE(int, int) {}

void SET_MOTION_CONTROL_MODE(CANON_MOTION_MODE mode, double tolerance, int,
                             double) {
  auto& state = recording::active();
  state.motion_mode = mode;
  state.motion_tolerance = tolerance;
}

void SET_NAIVECAM_TOLERANCE(double tolerance) {
  recording::active().naivecam_tolerance = tolerance;
}

void SET_CUTTER_RADIUS_COMPENSATION(double) {}
void START_CUTTER_RADIUS_COMPENSATION(int side) {
  recording::record(
      recording::CutterCompensation{static_cast<CANON_SIDE>(side)});
}

void STOP_CUTTER_RADIUS_COMPENSATION() {
  recording::record(recording::CutterCompensation{CANON_SIDE_OFF});
}
void START_SPEED_FEED_SYNCH(int, double, bool) {}
void STOP_SPEED_FEED_SYNCH() {}
void STOP() {}

void SET_SPINDLE_MODE(int, double) {}
void SPINDLE_RETRACT_TRAVERSE() {}
void SPINDLE_RETRACT() {}
void LOCK_SPINDLE_Z() {}
void USE_SPINDLE_FORCE() {}
void USE_NO_SPINDLE_FORCE() {}
void ORIENT_SPINDLE(int, double, int) {}
void WAIT_SPINDLE_ORIENT_COMPLETE(int, double) {}

void START_SPINDLE_CLOCKWISE(int spindle, int) {
  if (recording::valid_spindle(spindle))
    recording::active().spindle_direction[spindle] = CANON_CLOCKWISE;
}

void START_SPINDLE_COUNTERCLOCKWISE(int spindle, int) {
  if (recording::valid_spindle(spindle))
    recording::active().spindle_direction[spindle] = CANON_COUNTERCLOCKWISE;
}

void SET_SPINDLE_SPEED(int spindle, double speed) {
  if (recording::valid_spindle(spindle))
    recording::active().spindle_speed[spindle] = speed;
}

void STOP_SPINDLE_TURNING(int spindle, int) {
  if (recording::valid_spindle(spindle))
    recording::active().spindle_direction[spindle] = CANON_STOPPED;
}

void SET_JOINT_AVAILABILITY(int /*joint*/, int /*available*/) {}
void SET_SPINDLE_AVAILABILITY(int /*spindle*/, int /*available*/) {}

void USE_TOOL_LENGTH_OFFSET(const EmcPose& offset) {
  tool_offset = offset;
  recording::record(recording::ToolOffset{offset});
}

void SET_TOOL_TABLE_ENTRY(int, int, const EmcPose&, const EmcPose&, double,
                          double, double, int) {}

void SELECT_TOOL(int tool) { recording::active().selected_tool = tool; }

void CHANGE_TOOL() {
  auto& state = recording::active();
  state.tool_slot = state.selected_tool;
  recording::record(recording::ToolChange{state.selected_tool});
}

void CHANGE_TOOL_NUMBER(int number) {
  auto& state = recording::active();
  state.tool_slot = number;
  recording::record(recording::ToolChange{number});
}

void RELOAD_TOOLDATA() {}
void CLAMP_AXIS(CANON_AXIS) {}
void UNCLAMP_AXIS(CANON_AXIS) {}
void COMMENT(const char*) {}
void MESSAGE(char*) {}
void LOG(char*) {}
void LOGOPEN(char*) {}
void LOGAPPEND(char*) {}
void LOGCLOSE() {}
void DISABLE_ADAPTIVE_FEED() {}
void ENABLE_ADAPTIVE_FEED() {}
void DISABLE_FEED_OVERRIDE() {}
void ENABLE_FEED_OVERRIDE() {}
void DISABLE_SPEED_OVERRIDE(int) {}
void ENABLE_SPEED_OVERRIDE(int) {}
void LOCK_SPEED_OVERRIDE(int) {}
void DISABLE_FEED_HOLD() {}
void ENABLE_FEED_HOLD() {}
void FLOOD_OFF() { recording::active().flood = false; }
void FLOOD_ON() { recording::active().flood = true; }
void MIST_OFF() { recording::active().mist = false; }
void MIST_ON() { recording::active().mist = true; }
void PALLET_SHUTTLE() {}
void TURN_PROBE_OFF() {}
void TURN_PROBE_ON() {}
void NURB_KNOT_VECTOR() {}
void NURB_CONTROL_POINT(int, double, double, double, double) {}
void NURB_FEED(double, double) {}

void SET_BLOCK_DELETE(bool enabled) {
  recording::active().block_delete = enabled;
}
bool GET_BLOCK_DELETE() { return recording::active().block_delete; }
void OPTIONAL_PROGRAM_STOP() {}
void SET_OPTIONAL_PROGRAM_STOP(bool enabled) {
  recording::active().optional_stop = enabled;
}
bool GET_OPTIONAL_PROGRAM_STOP() { return recording::active().optional_stop; }
void PROGRAM_END() {}
void PROGRAM_STOP() {}
void SET_MOTION_OUTPUT_BIT(int) {}
void CLEAR_MOTION_OUTPUT_BIT(int) {}
void SET_AUX_OUTPUT_BIT(int) {}
void CLEAR_AUX_OUTPUT_BIT(int) {}
void SET_MOTION_OUTPUT_VALUE(int, double) {}
void SET_AUX_OUTPUT_VALUE(int, double) {}
int WAIT(int, int, int, double) { return 0; }
int UNLOCK_ROTARY(int, int) { return 0; }
int LOCK_ROTARY(int, int) { return 0; }

double GET_EXTERNAL_ANGLE_UNIT_FACTOR() { return 1.0; }
double GET_EXTERNAL_LENGTH_UNIT_FACTOR() { return 1.0; }
double GET_EXTERNAL_LENGTH_UNITS() {
  return 1.0 / recording::millimetres_per_unit(recording::active().units);
}
double GET_EXTERNAL_ANGLE_UNITS() { return 1.0; }
CANON_UNITS GET_EXTERNAL_LENGTH_UNIT_TYPE() {
  return recording::active().units;
}
double GET_EXTERNAL_AXIS_MAX_VELOCITY(int) { return 0.0; }
double GET_EXTERNAL_SPINDLE_MAX_VELOCITY(int) { return 0.0; }
double GET_EXTERNAL_FEED_RATE() { return recording::active().feed_rate; }
double GET_EXTERNAL_TRAVERSE_RATE() {
  return recording::active().traverse_rate;
}
int GET_EXTERNAL_FLOOD() { return recording::active().flood; }
int GET_EXTERNAL_MIST() { return recording::active().mist; }
CANON_MOTION_MODE GET_EXTERNAL_MOTION_CONTROL_MODE() {
  return recording::active().motion_mode;
}
int GET_EXTERNAL_KINS_TYPE() { return 0; }
int GET_EXTERNAL_KINS_TYPE_FLAGS(int) { return -1; }
double GET_EXTERNAL_MOTION_CONTROL_TOLERANCE() {
  return recording::active().motion_tolerance;
}
double GET_EXTERNAL_MOTION_CONTROL_NAIVECAM_TOLERANCE() {
  return recording::active().naivecam_tolerance;
}
double GET_EXTERNAL_ORIGIN_X() { return 0.0; }
double GET_EXTERNAL_ORIGIN_Y() { return 0.0; }
double GET_EXTERNAL_ORIGIN_Z() { return 0.0; }
double GET_EXTERNAL_ORIGIN_A() { return 0.0; }
double GET_EXTERNAL_ORIGIN_B() { return 0.0; }
double GET_EXTERNAL_ORIGIN_C() { return 0.0; }

void GET_EXTERNAL_PARAMETER_FILE_NAME(char* name, int max_size) {
  if (!name || max_size <= 0) return;
  std::strncpy(name, _parameter_file_name, max_size - 1);
  name[max_size - 1] = '\0';
}

void SET_PARAMETER_FILE_NAME(const char* name) {
  if (!name) return;
  std::strncpy(_parameter_file_name, name, PARAMETER_FILE_NAME_LENGTH - 1);
  _parameter_file_name[PARAMETER_FILE_NAME_LENGTH - 1] = '\0';
}

CANON_PLANE GET_EXTERNAL_PLANE() { return recording::active().plane; }
#define RECORDING_POSITION_GETTER(axis, member) \
  double GET_EXTERNAL_POSITION_##axis() {       \
    return recording::active().position.member; \
  }
RECORDING_POSITION_GETTER(X, tran.x)
RECORDING_POSITION_GETTER(Y, tran.y)
RECORDING_POSITION_GETTER(Z, tran.z)
RECORDING_POSITION_GETTER(A, a)
RECORDING_POSITION_GETTER(B, b)
RECORDING_POSITION_GETTER(C, c)
RECORDING_POSITION_GETTER(U, u)
RECORDING_POSITION_GETTER(V, v)
RECORDING_POSITION_GETTER(W, w)
#undef RECORDING_POSITION_GETTER

#define RECORDING_PROBE_GETTER(axis, member)          \
  double GET_EXTERNAL_PROBE_POSITION_##axis() {       \
    return recording::active().probe_position.member; \
  }
RECORDING_PROBE_GETTER(X, tran.x)
RECORDING_PROBE_GETTER(Y, tran.y)
RECORDING_PROBE_GETTER(Z, tran.z)
RECORDING_PROBE_GETTER(A, a)
RECORDING_PROBE_GETTER(B, b)
RECORDING_PROBE_GETTER(C, c)
RECORDING_PROBE_GETTER(U, u)
RECORDING_PROBE_GETTER(V, v)
RECORDING_PROBE_GETTER(W, w)
#undef RECORDING_PROBE_GETTER

double GET_EXTERNAL_PROBE_VALUE() { return 0.0; }
int GET_EXTERNAL_PROBE_TRIPPED_VALUE() { return 0; }
int GET_EXTERNAL_QUEUE_EMPTY() { return 1; }
double GET_EXTERNAL_SPEED(int spindle) {
  return recording::valid_spindle(spindle)
             ? recording::active().spindle_speed[spindle]
             : 0.0;
}
CANON_DIRECTION GET_EXTERNAL_SPINDLE(int spindle) {
  return recording::valid_spindle(spindle)
             ? recording::active().spindle_direction[spindle]
             : CANON_STOPPED;
}

double GET_EXTERNAL_TOOL_LENGTH_XOFFSET() { return tool_offset.tran.x; }
double GET_EXTERNAL_TOOL_LENGTH_YOFFSET() { return tool_offset.tran.y; }
double GET_EXTERNAL_TOOL_LENGTH_ZOFFSET() { return tool_offset.tran.z; }
double GET_EXTERNAL_TOOL_LENGTH_AOFFSET() { return tool_offset.a; }
double GET_EXTERNAL_TOOL_LENGTH_BOFFSET() { return tool_offset.b; }
double GET_EXTERNAL_TOOL_LENGTH_COFFSET() { return tool_offset.c; }
double GET_EXTERNAL_TOOL_LENGTH_UOFFSET() { return tool_offset.u; }
double GET_EXTERNAL_TOOL_LENGTH_VOFFSET() { return tool_offset.v; }
double GET_EXTERNAL_TOOL_LENGTH_WOFFSET() { return tool_offset.w; }
int GET_EXTERNAL_TOOL_SLOT() { return recording::active().tool_slot; }
int GET_EXTERNAL_SELECTED_TOOL_SLOT() {
  return recording::active().selected_tool;
}

CANON_TOOL_TABLE GET_EXTERNAL_TOOL_TABLE(int pocket) {
  auto& state = recording::active();
  if (pocket >= 0 && static_cast<std::size_t>(pocket) < state.tools.size())
    return state.tools[pocket];
  CANON_TOOL_TABLE result{};
  result.pocketno = -1;
  result.toolno = -1;
  return result;
}

int GET_EXTERNAL_TC_FAULT() { return 0; }
int GET_EXTERNAL_TC_REASON() { return 0; }
int GET_EXTERNAL_FEED_OVERRIDE_ENABLE() { return 1; }
int GET_EXTERNAL_SPINDLE_OVERRIDE_ENABLE(int) { return 1; }
int GET_EXTERNAL_ADAPTIVE_FEED_ENABLE() { return 0; }
int GET_EXTERNAL_FEED_HOLD_ENABLE() { return 1; }
int GET_EXTERNAL_DIGITAL_INPUT(int, int default_value) { return default_value; }
double GET_EXTERNAL_ANALOG_INPUT(int, double default_value) {
  return default_value;
}
int GET_EXTERNAL_AXIS_MASK() { return recording::active().axis_mask; }

int USER_DEFINED_FUNCTION_ADD(USER_DEFINED_FUNCTION_TYPE function, int index) {
  if (index < 0 || index >= USER_DEFINED_FUNCTION_NUM) return -1;
  USER_DEFINED_FUNCTION[index] = function;
  return 0;
}

void FINISH() {}
void START_G76_PASS() {}
void FINISH_G76_APPROACH() {}
void START_G76_CUT() {}
void START_G76_CLEARANCE() {}
void FINISH_G76_PASS(int) {}
void ON_RESET() {}
void CANON_ERROR(const char*, ...) {}
int GET_EXTERNAL_OFFSET_APPLIED() { return 0; }
EmcPose GET_EXTERNAL_OFFSETS() { return {}; }
void UPDATE_TAG(const StateTag&) {}
void SELECT_KINS_TYPE(int) {}
void INTERP_ABORT(int, const char*) {}

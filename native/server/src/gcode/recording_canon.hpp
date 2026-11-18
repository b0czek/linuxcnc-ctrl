// LinuxCNC CANON implementation that records a preview event stream.

#ifndef LINUXCNC_GRPC_GCODE_RECORDING_CANON_HPP
#define LINUXCNC_GRPC_GCODE_RECORDING_CANON_HPP

#include <memory>
#include <variant>
#include <vector>

#include "canon.hh"

namespace linuxcnc::recording {

struct SessionConfig {
  CANON_UNITS units = CANON_UNITS_INCHES;
  std::vector<CANON_TOOL_TABLE> tools;
  std::size_t max_events = 100000;
  std::size_t max_bytes = std::size_t{8} * 1024U * 1024U;
  std::size_t max_control_points = 10000;
};

struct StraightTraverse {
  int line_number;
  EmcPose end;
};

struct StraightFeed {
  int line_number;
  EmcPose end;
};

struct ArcFeed {
  int line_number;
  double first_end;
  double second_end;
  double first_axis;
  double second_axis;
  int rotation;
  double axis_end_point;
  double a;
  double b;
  double c;
  double u;
  double v;
  double w;
  CANON_PLANE plane;
};

struct StraightProbe {
  int line_number;
  EmcPose end;
  unsigned char probe_type;
};

struct RigidTap {
  int line_number;
  double x;
  double y;
  double z;
  double scale;
};

struct Dwell {
  double seconds;
};

struct NurbsG5Feed {
  int line_number;
  std::vector<NURBS_CONTROL_POINT> control_points;
  unsigned int order;
  CANON_PLANE plane;
};

struct NurbsG6Feed {
  int line_number;
  std::vector<NURBS_G6_CONTROL_POINT> control_points;
  unsigned int order;
  double feed_rate;
  int l_option;
  CANON_PLANE plane;
};

struct LengthUnits {
  CANON_UNITS units;
};

struct Plane {
  CANON_PLANE plane;
};

struct G5xOffset {
  int index;
  EmcPose offset;
};

struct G92Offset {
  EmcPose offset;
};

struct XyRotation {
  double degrees;
};

struct FeedRate {
  double rate;
};

struct CutterCompensation {
  CANON_SIDE side;
};

struct ToolOffset {
  EmcPose offset;
};

struct ToolChange {
  int tool_number;
};

using Event =
    std::variant<StraightTraverse, StraightFeed, ArcFeed, StraightProbe,
                 RigidTap, Dwell, NurbsG5Feed, NurbsG6Feed, LengthUnits, Plane,
                 G5xOffset, G92Offset, XyRotation, FeedRate, ToolOffset,
                 ToolChange, CutterCompensation>;

// A Session selects this canon implementation for one interpreter run and
// owns the resulting event stream. Canon calls without an active Session are
// programming errors; this keeps the invariant in one place instead of adding
// a nullable callback check to every motion function.
class Session {
 public:
  struct Impl;

  explicit Session(SessionConfig config = {});
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  std::vector<Event> take_events();
  bool limit_exceeded() const;

 private:
  std::unique_ptr<Impl> impl;
};

}  // namespace linuxcnc::recording

#endif  // LINUXCNC_GRPC_GCODE_RECORDING_CANON_HPP

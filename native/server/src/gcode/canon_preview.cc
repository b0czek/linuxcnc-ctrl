#include "linuxcnc_grpc/gcode/canon_preview.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "recording_canon.hpp"

namespace linuxcnc::server::gcode {
namespace {

namespace canon = ::linuxcnc::recording;
constexpr double pi = 3.14159265358979323846;
constexpr std::size_t max_batch_bytes =
    static_cast<std::size_t>(4U) * 1024U * 1024U;

std::size_t operation_size(const Operation& operation) {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NurbsG5Op>)
          return sizeof(T) + value.nurbsData.controlPoints.size() *
                                 sizeof(NurbsG5ControlPoint);
        else if constexpr (std::is_same_v<T, NurbsG6Op>)
          return sizeof(T) + value.nurbsData.controlPoints.size() *
                                 sizeof(NurbsG6ControlPoint);
        else
          return sizeof(T);
      },
      operation);
}

double positive_angle(double angle) {
  const double turn = 2.0 * pi;
  angle = std::fmod(angle, turn);
  return angle < 0.0 ? angle + turn : angle;
}

bool angle_is_on_arc(double angle, double start_angle, double sweep,
                     bool clockwise) {
  constexpr double epsilon = 1e-12;
  const double distance = clockwise ? positive_angle(start_angle - angle)
                                    : positive_angle(angle - start_angle);
  return distance <= sweep + epsilon;
}

Plane preview_plane(CANON_PLANE plane) {
  switch (plane) {
    case CANON_PLANE::XY:
      return Plane::XY;
    case CANON_PLANE::YZ:
      return Plane::YZ;
    case CANON_PLANE::XZ:
      return Plane::XZ;
    case CANON_PLANE::UV:
      return Plane::UV;
    case CANON_PLANE::VW:
      return Plane::VW;
    case CANON_PLANE::UW:
      return Plane::UW;
  }
  return Plane::XY;
}

CutterCompensationMode preview_cutter_compensation(CANON_SIDE side) {
  switch (side) {
    case CANON_SIDE_LEFT:
      return CutterCompensationMode::LEFT;
    case CANON_SIDE_RIGHT:
      return CutterCompensationMode::RIGHT;
    case CANON_SIDE_OFF:
      return CutterCompensationMode::OFF;
  }
  return CutterCompensationMode::OFF;
}

double linear_value(const ParseContext& context, double value) {
  return value * context.linearUnitScale;
}

Position preview_position(const ParseContext& context, const EmcPose& pose) {
  return {linear_value(context, pose.tran.x),
          linear_value(context, pose.tran.y),
          linear_value(context, pose.tran.z),
          pose.a,
          pose.b,
          pose.c,
          linear_value(context, pose.u),
          linear_value(context, pose.v),
          linear_value(context, pose.w)};
}

bool finite_position(const Position& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z) && std::isfinite(position.a) &&
         std::isfinite(position.b) && std::isfinite(position.c) &&
         std::isfinite(position.u) && std::isfinite(position.v) &&
         std::isfinite(position.w);
}

void require_finite(double value) {
  if (!std::isfinite(value))
    throw std::domain_error("non-finite canonical preview value");
}

void require_finite(const Position& position) {
  if (!finite_position(position))
    throw std::domain_error("non-finite canonical preview position");
}

void set_plane_end(Plane plane, double first, double second,
                   Position* position);

void update_control_point_extents(ParseContext& context, Plane plane,
                                  double first, double second) {
  Position point = context.currentPosition;
  set_plane_end(plane, first, second, &point);
  context.updateExtents(point);
}

void set_plane_end(Plane plane, double first, double second,
                   Position* position) {
  switch (plane) {
    case Plane::XY:
      position->x = first;
      position->y = second;
      break;
    case Plane::YZ:
      position->y = first;
      position->z = second;
      break;
    case Plane::XZ:
      position->z = first;
      position->x = second;
      break;
    case Plane::UV:
      position->u = first;
      position->v = second;
      break;
    case Plane::VW:
      position->v = first;
      position->w = second;
      break;
    case Plane::UW:
      position->u = first;
      position->w = second;
      break;
  }
}

void update_arc_extents(ParseContext& context, const Position& start,
                        const ArcOp& operation) {
  double start_first;
  double start_second;
  switch (operation.plane) {
    case Plane::XY:
      start_first = start.x;
      start_second = start.y;
      break;
    case Plane::YZ:
      start_first = start.y;
      start_second = start.z;
      break;
    case Plane::XZ:
      start_first = start.z;
      start_second = start.x;
      break;
    default:
      context.updateExtents(start);
      context.updateExtents(operation.pos);
      return;
  }
  const double first_offset = start_first - operation.arcData.centerFirst;
  const double second_offset = start_second - operation.arcData.centerSecond;
  const double radius = std::hypot(first_offset, second_offset);
  if (!std::isfinite(radius) || radius <= 0.0) {
    context.updateExtents(start);
    context.updateExtents(operation.pos);
    return;
  }
  double end_first;
  double end_second;
  switch (operation.plane) {
    case Plane::XY:
      end_first = operation.pos.x;
      end_second = operation.pos.y;
      break;
    case Plane::YZ:
      end_first = operation.pos.y;
      end_second = operation.pos.z;
      break;
    case Plane::XZ:
      end_first = operation.pos.z;
      end_second = operation.pos.x;
      break;
    default:
      return;
  }
  const double start_angle = std::atan2(second_offset, first_offset);
  const double end_angle =
      std::atan2(end_second - operation.arcData.centerSecond,
                 end_first - operation.arcData.centerFirst);
  const bool clockwise = operation.arcData.rotation < 0;
  double sweep = clockwise ? positive_angle(start_angle - end_angle)
                           : positive_angle(end_angle - start_angle);
  const int turn_count = std::abs(operation.arcData.rotation);
  if (sweep == 0.0)
    sweep = 2.0 * pi;
  else if (turn_count >= 1)
    sweep += (turn_count - 1) * 2.0 * pi;

  context.updateExtents(start);
  context.updateExtents(operation.pos);
  constexpr double cardinals[] = {0.0, pi / 2.0, pi, 3.0 * pi / 2.0};
  for (const double angle : cardinals) {
    if (!angle_is_on_arc(angle, start_angle, sweep, clockwise)) continue;
    Position point = start;
    set_plane_end(operation.plane,
                  operation.arcData.centerFirst + radius * std::cos(angle),
                  operation.arcData.centerSecond + radius * std::sin(angle),
                  &point);
    context.updateExtents(point);
  }
}

void consume(ParseContext& context, const canon::StraightTraverse& event) {
  TraverseOp operation;
  operation.lineNumber = event.line_number;
  operation.pos = preview_position(context, event.end);
  context.updateExtents(context.currentPosition);
  context.currentPosition = operation.pos;
  context.updateExtents(operation.pos);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::StraightFeed& event) {
  FeedOp operation;
  operation.lineNumber = event.line_number;
  operation.pos = preview_position(context, event.end);
  context.updateExtents(context.currentPosition);
  context.currentPosition = operation.pos;
  context.updateExtents(operation.pos);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::ArcFeed& event) {
  ArcOp operation;
  operation.lineNumber = event.line_number;
  operation.plane = preview_plane(event.plane);
  operation.pos = context.currentPosition;
  const double first_end = linear_value(context, event.first_end);
  const double second_end = linear_value(context, event.second_end);
  const double axis_end = linear_value(context, event.axis_end_point);
  switch (operation.plane) {
    case Plane::XY:
      operation.pos.x = first_end;
      operation.pos.y = second_end;
      operation.pos.z = axis_end;
      break;
    case Plane::YZ:
      operation.pos.y = first_end;
      operation.pos.z = second_end;
      operation.pos.x = axis_end;
      break;
    case Plane::XZ:
      operation.pos.z = first_end;
      operation.pos.x = second_end;
      operation.pos.y = axis_end;
      break;
    default:
      set_plane_end(operation.plane, first_end, second_end, &operation.pos);
      break;
  }
  operation.pos.a = event.a;
  operation.pos.b = event.b;
  operation.pos.c = event.c;
  operation.pos.u = linear_value(context, event.u);
  operation.pos.v = linear_value(context, event.v);
  operation.pos.w = linear_value(context, event.w);
  operation.arcData.centerFirst = linear_value(context, event.first_axis);
  operation.arcData.centerSecond = linear_value(context, event.second_axis);
  operation.arcData.rotation = event.rotation;
  operation.arcData.axisEndPoint = axis_end;
  update_arc_extents(context, context.currentPosition, operation);
  context.currentPosition = operation.pos;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::StraightProbe& event) {
  ProbeOp operation;
  operation.lineNumber = event.line_number;
  operation.pos = preview_position(context, event.end);
  context.updateExtents(context.currentPosition);
  context.currentPosition = operation.pos;
  context.updateExtents(operation.pos);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::RigidTap& event) {
  RigidTapOp operation;
  operation.lineNumber = event.line_number;
  operation.pos = {linear_value(context, event.x),
                   linear_value(context, event.y),
                   linear_value(context, event.z)};
  operation.scale = event.scale;
  context.updateExtents(context.currentPosition);
  context.currentPosition.x = operation.pos.x;
  context.currentPosition.y = operation.pos.y;
  context.extents.update(operation.pos.x, operation.pos.y, operation.pos.z);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::Dwell& event) {
  DwellOp operation;
  operation.pos = context.currentPosition;
  operation.duration = event.seconds;
  operation.plane = context.currentPlane;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::NurbsG5Feed& event) {
  NurbsG5Op operation;
  operation.lineNumber = event.line_number;
  operation.plane = preview_plane(event.plane);
  operation.nurbsData.order = event.order;
  operation.nurbsData.controlPoints.reserve(event.control_points.size());
  for (const auto& source : event.control_points)
    operation.nurbsData.controlPoints.push_back(
        {linear_value(context, source.NURBS_X),
         linear_value(context, source.NURBS_Y), source.NURBS_W});
  operation.pos = context.currentPosition;
  context.updateExtents(context.currentPosition);
  if (!operation.nurbsData.controlPoints.empty()) {
    for (const auto& point : operation.nurbsData.controlPoints)
      update_control_point_extents(context, operation.plane, point.x, point.y);
    const auto& last = operation.nurbsData.controlPoints.back();
    set_plane_end(operation.plane, last.x, last.y, &operation.pos);
    context.currentPosition = operation.pos;
    context.updateExtents(operation.pos);
  }
  context.addOperation(std::move(operation));
}

void consume(ParseContext& context, const canon::NurbsG6Feed& event) {
  NurbsG6Op operation;
  operation.lineNumber = event.line_number;
  operation.plane = preview_plane(event.plane);
  operation.nurbsData.order = event.order;
  operation.nurbsData.interpolationMethod = event.l_option;
  operation.nurbsData.controlPoints.reserve(event.control_points.size());
  for (const auto& source : event.control_points)
    operation.nurbsData.controlPoints.push_back(
        {linear_value(context, source.NURBS_X),
         linear_value(context, source.NURBS_Y), source.NURBS_R,
         source.NURBS_K});
  operation.pos = context.currentPosition;
  context.updateExtents(context.currentPosition);
  if (operation.nurbsData.controlPoints.size() > event.order) {
    for (const auto& point : operation.nurbsData.controlPoints)
      update_control_point_extents(context, operation.plane, point.x, point.y);
    const auto& last = operation.nurbsData.controlPoints.back();
    set_plane_end(operation.plane, last.x, last.y, &operation.pos);
    context.currentPosition = operation.pos;
    context.updateExtents(operation.pos);
  }
  context.addOperation(std::move(operation));
}

void consume(ParseContext& context, const canon::LengthUnits& event) {
  Units next_units = Units::MM;
  switch (event.units) {
    case CANON_UNITS_INCHES:
      next_units = Units::INCHES;
      context.linearUnitScale = 25.4;
      break;
    case CANON_UNITS_MM:
      next_units = Units::MM;
      context.linearUnitScale = 1.0;
      break;
    case CANON_UNITS_CM:
      next_units = Units::CM;
      context.linearUnitScale = 10.0;
      break;
  }
  if (next_units == context.currentUnits) return;
  context.currentUnits = next_units;
  UnitsChangeOp operation;
  operation.units = next_units;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::Plane& event) {
  const Plane next_plane = preview_plane(event.plane);
  if (next_plane == context.currentPlane) return;
  context.currentPlane = next_plane;
  PlaneChangeOp operation;
  operation.plane = next_plane;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::G5xOffset& event) {
  G5xOffsetOp operation;
  operation.origin = event.index;
  operation.offset = preview_position(context, event.offset);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::G92Offset& event) {
  G92OffsetOp operation;
  operation.offset = preview_position(context, event.offset);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::XyRotation& event) {
  XYRotationOp operation;
  operation.rotation = event.degrees;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::FeedRate& event) {
  const double rate = linear_value(context, event.rate);
  context.currentFeedRate = rate;
  if (rate == context.lastFeedRate) return;
  context.lastFeedRate = rate;
  FeedRateChangeOp operation;
  operation.feedRate = rate;
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::CutterCompensation& event) {
  CutterCompensationChangeOp operation;
  operation.mode = preview_cutter_compensation(event.side);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::ToolOffset& event) {
  ToolOffsetOp operation;
  operation.offset = preview_position(context, event.offset);
  context.addOperation(operation);
}

void consume(ParseContext& context, const canon::ToolChange& event) {
  context.selectedTool = event.tool_number;
  ToolChangeOp operation;
  operation.toolNumber = event.tool_number;
  context.addOperation(operation);
}

}  // namespace

void ParseContext::addOperation(Operation&& operation) {
  if (stopToken.stop_requested()) return;
  std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TraverseOp> ||
                      std::is_same_v<T, FeedOp> || std::is_same_v<T, ArcOp> ||
                      std::is_same_v<T, ProbeOp> ||
                      std::is_same_v<T, DwellOp> ||
                      std::is_same_v<T, NurbsG5Op> ||
                      std::is_same_v<T, NurbsG6Op>) {
          require_finite(value.pos);
        }
        if constexpr (std::is_same_v<T, ArcOp>) {
          require_finite(value.arcData.centerFirst);
          require_finite(value.arcData.centerSecond);
          require_finite(value.arcData.axisEndPoint);
        } else if constexpr (std::is_same_v<T, RigidTapOp>) {
          require_finite(value.pos.x);
          require_finite(value.pos.y);
          require_finite(value.pos.z);
          require_finite(value.scale);
        } else if constexpr (std::is_same_v<T, DwellOp>) {
          require_finite(value.duration);
        } else if constexpr (std::is_same_v<T, NurbsG5Op>) {
          for (const auto& point : value.nurbsData.controlPoints) {
            require_finite(point.x);
            require_finite(point.y);
            require_finite(point.weight);
          }
        } else if constexpr (std::is_same_v<T, NurbsG6Op>) {
          for (const auto& point : value.nurbsData.controlPoints) {
            require_finite(point.x);
            require_finite(point.y);
            require_finite(point.r);
            require_finite(point.k);
          }
        } else if constexpr (std::is_same_v<T, G5xOffsetOp> ||
                             std::is_same_v<T, G92OffsetOp> ||
                             std::is_same_v<T, ToolOffsetOp>) {
          require_finite(value.offset);
        } else if constexpr (std::is_same_v<T, XYRotationOp>) {
          require_finite(value.rotation);
        } else if constexpr (std::is_same_v<T, FeedRateChangeOp>) {
          require_finite(value.feedRate);
        }
      },
      operation);
  operations.push_back(std::move(operation));
  ++operationCount;
}

void ParseContext::flushReadyBatch() {
  if (!batchCallback || batchSize == 0 || operations.size() < batchSize) return;
  flushBatch();
}

void ParseContext::flushBatch() {
  if (!batchCallback || operations.empty()) return;
  const std::size_t max_batch = batchSize == 0 ? operations.size() : batchSize;
  std::size_t offset = 0;
  while (offset < operations.size() && !stopToken.stop_requested()) {
    std::size_t count = 0;
    std::size_t bytes = 0;
    while (offset + count < operations.size() && count < max_batch) {
      const auto next = operation_size(operations[offset + count]);
      if (count != 0 && bytes + next > max_batch_bytes) break;
      if (next > max_batch_bytes)
        throw std::runtime_error(
            "G-code operation exceeds preview batch byte limit");
      bytes += next;
      ++count;
    }
    OperationBatch batch;
    batch.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
      batch.push_back(std::move(operations[offset + index]));
    offset += count;
    batchCallback(std::move(batch));
  }
  operations.erase(operations.begin(),
                   operations.begin() + static_cast<std::ptrdiff_t>(offset));
}

void ParseContext::updateExtents(const Position& position) {
  require_finite(position);
  extents.update(position);
}

void ParseContext::reportProgress(std::size_t bytes_read) {
  if (!progressCallback || totalBytes == 0) return;
  ParseProgress progress;
  progress.bytesRead = bytes_read;
  progress.totalBytes = totalBytes;
  progress.percent =
      (static_cast<double>(bytes_read) / static_cast<double>(totalBytes)) *
      100.0;
  progress.operationCount = operationCount;
  progressCallback(progress);
}

void consumeRecordingEvents(::linuxcnc::recording::Session& session,
                            ParseContext& context) {
  for (const auto& event : session.take_events())
    std::visit([&context](const auto& value) { consume(context, value); },
               event);
}

}  // namespace linuxcnc::server::gcode

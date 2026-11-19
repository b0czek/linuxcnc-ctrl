#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "linuxcnc_grpc/gcode/parser.hpp"

using linuxcnc::server::gcode::ArcOp;
using linuxcnc::server::gcode::CutterCompensationChangeOp;
using linuxcnc::server::gcode::CutterCompensationMode;
using linuxcnc::server::gcode::DwellOp;
using linuxcnc::server::gcode::FeedOp;
using linuxcnc::server::gcode::FeedRateChangeOp;
using linuxcnc::server::gcode::G5xOffsetOp;
using linuxcnc::server::gcode::G92OffsetOp;
using linuxcnc::server::gcode::Operation;
using linuxcnc::server::gcode::OperationBatch;
using linuxcnc::server::gcode::ParseError;
using linuxcnc::server::gcode::ParseOptions;
using linuxcnc::server::gcode::PlaneChangeOp;
using linuxcnc::server::gcode::SerializedRs274Parser;
using linuxcnc::server::gcode::TraverseOp;
using linuxcnc::server::gcode::UnitsChangeOp;

namespace {

template <typename T>
std::size_t count_operations(const std::vector<Operation>& operations) {
  return static_cast<std::size_t>(std::count_if(
      operations.begin(), operations.end(),
      [](const auto& op) { return std::holds_alternative<T>(op); }));
}

bool nearly_equal(double actual, double expected) {
  return std::abs(actual - expected) < 1e-9;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 10) {
    std::cerr << "usage: gcode_parser_integration <ini-path> <gcode-path> "
                 "<operations-gcode-path> <cutter-comp-gcode-path> "
                 "<python-remap-gcode-path> <modal-free-metric-path> "
                 "<missing-linear-units-ini> <xyzau-ini> "
                 "<rotary-axis-gcode-path>\n";
    return 2;
  }

  std::size_t delivered = 0;
  std::size_t operations = 0;
  SerializedRs274Parser parser;
  ParseOptions options;
  options.ini_path = argv[1];
  options.batch_size = 2;
  options.on_batch = [&](OperationBatch&& batch) {
    assert(!batch.empty());
    assert(batch.size() <= options.batch_size);
    operations += batch.size();
    ++delivered;
  };

  const auto parsed = parser.parse_file(argv[2], options);
  assert(!parsed.cancelled);
  assert(parsed.operationCount == operations);
  assert(parsed.operationCount > 0);
  assert(delivered > 0);
  assert(parsed.extents.isValid());

  std::vector<Operation> preview_operations;
  ParseOptions preview_options;
  preview_options.ini_path = argv[1];
  preview_options.batch_size = 3;
  preview_options.on_batch = [&](OperationBatch&& batch) {
    preview_operations.insert(preview_operations.end(),
                              std::make_move_iterator(batch.begin()),
                              std::make_move_iterator(batch.end()));
  };
  const auto preview = parser.parse_file(argv[3], preview_options);
  assert(!preview.cancelled);
  assert(preview.operationCount == preview_operations.size());
  assert(count_operations<TraverseOp>(preview_operations) >= 2);
  assert(count_operations<FeedOp>(preview_operations) >= 3);
  assert(count_operations<ArcOp>(preview_operations) == 2);
  assert(count_operations<DwellOp>(preview_operations) == 1);
  assert(count_operations<G5xOffsetOp>(preview_operations) >= 1);
  assert(count_operations<G92OffsetOp>(preview_operations) >= 1);
  assert(count_operations<PlaneChangeOp>(preview_operations) >= 2);
  // Interpreter initialization may report its configured unit system before
  // the program's explicit G21. The G20/G21 pair must add at least two unit
  // transitions in every case.
  assert(count_operations<UnitsChangeOp>(preview_operations) >= 2);
  assert(count_operations<FeedRateChangeOp>(preview_operations) >= 2);
  assert(preview.extents.isValid());
  assert(preview.extents.max.x >= 25.4);
  assert(preview.extents.max.y >= 50.8);

  const auto inch_feed = std::find_if(
      preview_operations.begin(), preview_operations.end(), [](const auto& op) {
        const auto* feed = std::get_if<FeedOp>(&op);
        return feed && nearly_equal(feed->pos.x, 25.4) &&
               nearly_equal(feed->pos.y, 50.8);
      });
  assert(inch_feed != preview_operations.end());

  // Cutter compensation is resolved by the interpreter before canonical
  // output. The preview therefore receives the compensated tool-center path:
  // a 2 mm diameter left compensation offsets these legs by a 1 mm radius.
  std::vector<Operation> compensated_operations;
  ParseOptions compensated_options;
  compensated_options.ini_path = argv[1];
  compensated_options.on_batch = [&](OperationBatch&& batch) {
    compensated_operations.insert(compensated_operations.end(),
                                  std::make_move_iterator(batch.begin()),
                                  std::make_move_iterator(batch.end()));
  };
  const auto compensated = parser.parse_file(argv[4], compensated_options);
  assert(!compensated.cancelled);
  assert(compensated.operationCount == compensated_operations.size());
  const auto compensated_horizontal =
      std::find_if(compensated_operations.begin(), compensated_operations.end(),
                   [](const auto& op) {
                     const auto* feed = std::get_if<FeedOp>(&op);
                     return feed && nearly_equal(feed->pos.x, 9.0) &&
                            nearly_equal(feed->pos.y, 1.0);
                   });
  const auto compensated_vertical =
      std::find_if(compensated_operations.begin(), compensated_operations.end(),
                   [](const auto& op) {
                     const auto* feed = std::get_if<FeedOp>(&op);
                     return feed && nearly_equal(feed->pos.x, 9.0) &&
                            nearly_equal(feed->pos.y, 10.0);
                   });
  assert(compensated_horizontal != compensated_operations.end());
  assert(compensated_vertical != compensated_operations.end());
  const auto compensation_on = std::find_if(
      compensated_operations.begin(), compensated_operations.end(),
      [](const auto& op) {
        const auto* change = std::get_if<CutterCompensationChangeOp>(&op);
        return change && change->mode == CutterCompensationMode::LEFT;
      });
  const auto compensation_off = std::find_if(
      compensation_on, compensated_operations.end(), [](const auto& op) {
        const auto* change = std::get_if<CutterCompensationChangeOp>(&op);
        return change && change->mode == CutterCompensationMode::OFF;
      });
  assert(compensation_on != compensated_operations.end());
  assert(compensation_on < compensated_horizontal);
  assert(compensated_vertical < compensation_off);
  assert(compensation_off != compensated_operations.end());
  const auto compensation_right = std::find_if(
      compensation_off, compensated_operations.end(), [](const auto& op) {
        const auto* change = std::get_if<CutterCompensationChangeOp>(&op);
        return change && change->mode == CutterCompensationMode::RIGHT;
      });
  const auto final_compensation_off = std::find_if(
      compensation_right, compensated_operations.end(), [](const auto& op) {
        const auto* change = std::get_if<CutterCompensationChangeOp>(&op);
        return change && change->mode == CutterCompensationMode::OFF;
      });
  assert(compensation_right != compensated_operations.end());
  assert(final_compensation_off != compensated_operations.end());

  // Both self.execute() and direct emccanon calls in a Python remap use the
  // same recording canon as ordinary G-code. No callback into the RPC adapter
  // is involved, and self.task == 0 keeps LinuxCNC's preview semantics.
  std::vector<Operation> remap_operations;
  ParseOptions remap_options;
  remap_options.ini_path = argv[1];
  remap_options.on_batch = [&](OperationBatch&& batch) {
    remap_operations.insert(remap_operations.end(),
                            std::make_move_iterator(batch.begin()),
                            std::make_move_iterator(batch.end()));
  };
  const auto remapped = parser.parse_file(argv[5], remap_options);
  assert(!remapped.cancelled);
  assert(remapped.operationCount == remap_operations.size());
  const auto self_execute_feed = std::find_if(
      remap_operations.begin(), remap_operations.end(), [](const auto& op) {
        const auto* feed = std::get_if<FeedOp>(&op);
        return feed && nearly_equal(feed->pos.x, 12.0) &&
               nearly_equal(feed->pos.y, 3.0);
      });
  const auto direct_canon_feed = std::find_if(
      remap_operations.begin(), remap_operations.end(), [](const auto& op) {
        const auto* feed = std::get_if<FeedOp>(&op);
        return feed && feed->lineNumber == 405 &&
               nearly_equal(feed->pos.x, 20.0) &&
               nearly_equal(feed->pos.y, 4.0);
      });
  assert(self_execute_feed != remap_operations.end());
  assert(direct_canon_feed != remap_operations.end());

  std::vector<Operation> modal_free_operations;
  ParseOptions modal_free_options;
  modal_free_options.ini_path = argv[1];
  modal_free_options.on_batch = [&](OperationBatch&& batch) {
    modal_free_operations.insert(modal_free_operations.end(),
                                 std::make_move_iterator(batch.begin()),
                                 std::make_move_iterator(batch.end()));
  };
  parser.parse_file(argv[6], modal_free_options);
  const auto metric_move =
      std::find_if(modal_free_operations.begin(), modal_free_operations.end(),
                   [](const auto& op) {
                     const auto* traverse = std::get_if<TraverseOp>(&op);
                     return traverse && nearly_equal(traverse->pos.x, 1.0);
                   });
  assert(metric_move != modal_free_operations.end());

  // Preserve the pre-refactor policy: an INI without LINEAR_UNITS starts the
  // preview canon in inches. A modal-free X1 move therefore lands at 25.4 mm.
  std::vector<Operation> default_unit_operations;
  ParseOptions default_unit_options;
  default_unit_options.ini_path = argv[7];
  default_unit_options.on_batch = [&](OperationBatch&& batch) {
    default_unit_operations.insert(default_unit_operations.end(),
                                   std::make_move_iterator(batch.begin()),
                                   std::make_move_iterator(batch.end()));
  };
  parser.parse_file(argv[6], default_unit_options);
  const auto default_inch_move =
      std::find_if(default_unit_operations.begin(),
                   default_unit_operations.end(), [](const auto& op) {
                     const auto* traverse = std::get_if<TraverseOp>(&op);
                     return traverse && nearly_equal(traverse->pos.x, 25.4);
                   });
  assert(default_inch_move != default_unit_operations.end());

  // The preview accepts exactly the axes [TRAJ]COORDINATES configures, as task
  // does: an A and U move parses with X Y Z A U and is refused with X Y Z.
  std::vector<Operation> rotary_operations;
  ParseOptions rotary_options;
  rotary_options.ini_path = argv[8];
  rotary_options.on_batch = [&](OperationBatch&& batch) {
    rotary_operations.insert(rotary_operations.end(),
                             std::make_move_iterator(batch.begin()),
                             std::make_move_iterator(batch.end()));
  };
  parser.parse_file(argv[9], rotary_options);
  const auto rotary_move = std::find_if(
      rotary_operations.begin(), rotary_operations.end(), [](const auto& op) {
        const auto* traverse = std::get_if<TraverseOp>(&op);
        return traverse && nearly_equal(traverse->pos.x, 1.0) &&
               nearly_equal(traverse->pos.a, 90.0) &&
               nearly_equal(traverse->pos.u, 2.0);
      });
  assert(rotary_move != rotary_operations.end());

  bool unconfigured_axis_refused = false;
  ParseOptions xyz_options;
  xyz_options.ini_path = argv[1];
  try {
    parser.parse_file(argv[9], xyz_options);
  } catch (const ParseError& error) {
    unconfigured_axis_refused =
        std::string(error.what()).find("Bad character 'a'") !=
        std::string::npos;
  }
  assert(unconfigured_axis_refused);

  // A callback can cancel after a bounded batch. Cancellation is observed
  // before the next rs274 read/execute step and never removes that batch.
  std::size_t cancelled_batches = 0;
  std::stop_source stop_source;
  ParseOptions cancelled_options;
  cancelled_options.ini_path = argv[1];
  cancelled_options.batch_size = 1;
  cancelled_options.on_batch = [&](OperationBatch&& batch) {
    assert(batch.size() == 1);
    ++cancelled_batches;
    stop_source.request_stop();
  };
  cancelled_options.stop_token = stop_source.get_token();
  const auto cancelled = parser.parse_file(argv[2], cancelled_options);
  assert(cancelled.cancelled);
  // A single interpreter execute step may emit more than one canonical
  // operation; cancellation is intentionally checked at the step boundary.
  assert(cancelled_batches >= 1);
  assert(cancelled.operationCount >= 1);
  return 0;
}

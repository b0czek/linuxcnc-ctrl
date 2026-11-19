#include "linuxcnc_grpc/gcode/parser.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "interp_base.hh"
#include "interp_return.hh"
#include "linuxcnc_grpc/gcode/canon_preview.hpp"
#include "linuxcnc_grpc/linuxcnc/active_ini.hpp"
#include "recording_canon.hpp"
#include "rs274ngc_interp.hh"
#include "tooldata.hh"

namespace linuxcnc::server::gcode {
namespace {

std::mutex rs274_mutex;

bool result_ok(const int result) {
  return result == INTERP_OK || result == INTERP_EXECUTE_FINISH;
}

struct IniPreviewState {
  CANON_UNITS units = CANON_UNITS_INCHES;
  int axis_mask = 0x7;
  std::string startup_code;
};

// The axis mask exactly as task builds it (src/emc/ini/initraj.cc): every
// letter of XYZABCUVW named in [TRAJ]COORDINATES, in any case, and X, Y and Z
// when COORDINATES is missing. Other characters, such as spaces, are ignored.
int axis_mask_from_coordinates(const std::optional<std::string>& coordinates) {
  if (!coordinates) return 0x7;
  constexpr std::string_view axes = "XYZABCUVW";
  int mask = 0;
  for (const char character : *coordinates) {
    const auto index = axes.find(
        static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    if (index != std::string_view::npos) mask |= 1 << index;
  }
  return mask;
}

IniPreviewState load_preview_state(const std::string& path) {
  const ActiveIni ini(path);
  IniPreviewState state;
  const auto units = ini.find_string("TRAJ", "LINEAR_UNITS");
  if (units) {
    auto normalized_units = *units;
    std::transform(normalized_units.begin(), normalized_units.end(),
                   normalized_units.begin(), [](unsigned char character) {
                     return std::tolower(character);
                   });
    if (normalized_units == "mm" || normalized_units == "metric")
      state.units = CANON_UNITS_MM;
    else if (normalized_units == "cm")
      state.units = CANON_UNITS_CM;
    else if (normalized_units == "inch" || normalized_units == "in" ||
             normalized_units == "imperial")
      state.units = CANON_UNITS_INCHES;
    else
      throw std::runtime_error("unsupported [TRAJ]LINEAR_UNITS: " + *units);
  }
  state.axis_mask =
      axis_mask_from_coordinates(ini.find_string("TRAJ", "COORDINATES"));
  if (const auto startup = ini.find_string("RS274NGC", "RS274NGC_STARTUP_CODE"))
    state.startup_code = *startup;
  return state;
}

}  // namespace

void ensure_python_modules_linked() noexcept;

SerializedRs274Parser::SerializedRs274Parser() = default;

SerializedRs274Parser::~SerializedRs274Parser() = default;

ParseResult SerializedRs274Parser::parse_file(const std::string& filepath,
                                              const ParseOptions& options) {
  if (options.ini_path.empty())
    throw std::invalid_argument("G-code parser requires a LinuxCNC INI path");

  std::lock_guard<std::mutex> lock(rs274_mutex);
  ensure_python_modules_linked();

  // Interp::ini_load() loads the parameter file name; the rest of LinuxCNC's
  // interpreter configuration (including REMAP and PYTHON) is intentionally
  // discovered through INI_FILE_NAME during init(). Parsing is serialized, so
  // this process-global LinuxCNC convention cannot race another session.
  // NOLINTNEXTLINE(concurrency-mt-unsafe): serialized external API contract
  if (setenv("INI_FILE_NAME", options.ini_path.c_str(), 1) != 0)
    throw std::runtime_error("failed to set LinuxCNC INI_FILE_NAME");

  struct stat file_stat{};
  if (stat(filepath.c_str(), &file_stat) != 0)
    throw ParseError(ParseErrorCode::InvalidEntry,
                     "G-code file not found: " + filepath);
  if (!S_ISREG(file_stat.st_mode))
    throw ParseError(ParseErrorCode::InvalidEntry,
                     "G-code path is not a regular file: " + filepath);

  ParseContext context;
  context.totalBytes = static_cast<std::size_t>(file_stat.st_size);
  context.extents.reset();
  context.progressCallback = options.on_progress;
  context.batchCallback = options.on_batch;
  context.stopToken = options.stop_token;
  context.batchSize = options.batch_size;

  const auto preview_state = load_preview_state(options.ini_path);
  ::linuxcnc::recording::SessionConfig session_config;
  session_config.units = preview_state.units;
  session_config.axis_mask = preview_state.axis_mask;
  session_config.tools.assign(CANON_POCKETS_MAX, tooldata_entry_init());
  if (tool_mmap_user() == 0) {
    for (int pocket = 0; pocket < CANON_POCKETS_MAX; ++pocket) {
      if (tooldata_get(&session_config.tools[pocket], pocket) != IDX_OK)
        session_config.tools[pocket] = tooldata_entry_init();
    }
  }

  // The selected canon backend records every call made by the interpreter,
  // including calls from Python/NGC remaps. Translation and RPC batching stay
  // outside the interpreter/canonical call stack.
  ::linuxcnc::recording::Session recording_session(std::move(session_config));
  bool opened = false;

  try {
    if (!interpreter_) interpreter_.reset(makeInterp());
    if (!interpreter_)
      throw std::runtime_error("failed to create rs274 interpreter");

    // The daemon supplies its configured INI explicitly for every parse. The
    // interpreter is reinitialized even when the path is unchanged so modal
    // state never leaks from a previous workspace/program.
    if (loaded_ini_path_ != options.ini_path) {
      if (interpreter_->ini_load(options.ini_path.c_str()) != 0)
        throw std::runtime_error("failed to load INI file: " +
                                 options.ini_path);
      loaded_ini_path_ = options.ini_path;
    }
    if (interpreter_->init() != 0)
      throw std::runtime_error("failed to initialize rs274 interpreter");
    if (!options.program_prefix.empty()) {
      auto* concrete = dynamic_cast<Interp*>(interpreter_.get());
      if (!concrete)
        throw std::runtime_error(
            "rs274 interpreter does not expose its program prefix");
      if (options.program_prefix.size() >=
          sizeof(concrete->_setup.program_prefix))
        throw std::runtime_error("G-code program prefix is too long");
      std::copy(options.program_prefix.begin(), options.program_prefix.end(),
                concrete->_setup.program_prefix);
      concrete->_setup.program_prefix[options.program_prefix.size()] = '\0';
    }
    consumeRecordingEvents(recording_session, context);
    if (recording_session.limit_exceeded())
      throw std::runtime_error("G-code preview resource limit exceeded");
    if (!preview_state.startup_code.empty()) {
      const int startup_result =
          interpreter_->execute(preview_state.startup_code.c_str());
      consumeRecordingEvents(recording_session, context);
      if (recording_session.limit_exceeded())
        throw std::runtime_error("G-code preview resource limit exceeded");
      if (!result_ok(startup_result)) {
        char error_buffer[256]{};
        interpreter_->error_text(startup_result, error_buffer,
                                 sizeof(error_buffer));
        throw std::runtime_error(
            std::string("invalid RS274NGC_STARTUP_CODE: ") + error_buffer);
      }
    }

    if (interpreter_->open(filepath.c_str()) != 0)
      throw ParseError(ParseErrorCode::InvalidEntry,
                       "failed to open G-code file: " + filepath);
    opened = true;

    const std::size_t estimated_lines =
        std::max(context.totalBytes / 25U, std::size_t{100});
    const std::size_t progress_interval =
        options.progress_updates > 0
            ? std::max(estimated_lines /
                           static_cast<std::size_t>(options.progress_updates),
                       std::size_t{1})
            : static_cast<std::size_t>(-1);

    int result = INTERP_OK;
    std::size_t line_count = 0;
    while (result_ok(result)) {
      // This check is deliberately before read() and after execute(). It lets
      // RPC cancellation stop the wait without undoing an already accepted
      // interpreter step.
      if (context.stopToken.stop_requested()) break;

      result = interpreter_->read();
      if (context.stopToken.stop_requested()) break;
      if (!result_ok(result)) break;

      result = interpreter_->execute();
      ++line_count;
      consumeRecordingEvents(recording_session, context);
      if (recording_session.limit_exceeded())
        throw std::runtime_error("G-code preview resource limit exceeded");
      if (!context.stopToken.stop_requested()) {
        // Delivery happens after execute returns, outside canonical callback
        // code, so a gRPC adapter can enqueue work without doing network I/O
        // from the interpreter/canonical path.
        context.flushReadyBatch();
      }
      if (context.stopToken.stop_requested()) break;

      if (context.progressCallback && line_count % progress_interval == 0) {
        std::size_t estimated_bytes =
            (context.totalBytes * line_count) /
            std::max(line_count + 100U, std::size_t{1});
        estimated_bytes = std::min(estimated_bytes, context.totalBytes);
        context.reportProgress(estimated_bytes);
      }
    }

    if (!context.stopToken.stop_requested() && result != INTERP_ENDFILE &&
        result != INTERP_EXIT && !result_ok(result)) {
      char error_buffer[256]{};
      interpreter_->error_text(result, error_buffer, sizeof(error_buffer));
      const int line = interpreter_->sequence_number();
      throw ParseError(ParseErrorCode::Interpreter,
                       std::string("G-code parse error: ") + error_buffer,
                       line > 0 ? std::optional<int>(line) : std::nullopt);
    }

    interpreter_->close();
    opened = false;

    // Flush the final partial batch only if the consumer is still connected.
    // A cancelled stream must not retain or deliver an unbounded tail.
    if (!context.stopToken.stop_requested()) {
      consumeRecordingEvents(recording_session, context);
      context.flushBatch();
    }
    if (context.progressCallback) context.reportProgress(context.totalBytes);
  } catch (const std::domain_error& error) {
    const int line = interpreter_ ? interpreter_->sequence_number() : 0;
    if (opened) interpreter_->close();
    throw ParseError(ParseErrorCode::Interpreter, error.what(),
                     line > 0 ? std::optional<int>(line) : std::nullopt);
  } catch (...) {
    if (opened) interpreter_->close();
    throw;
  }

  if (!context.extents.isValid()) {
    context.extents.min = {0, 0, 0};
    context.extents.max = {0, 0, 0};
  }

  ParseResult result;
  result.operations = std::move(context.operations);
  result.extents = context.extents;
  result.operationCount = context.operationCount;
  result.cancelled = context.stopToken.stop_requested();
  return result;
}

ParseResult parse_file(const std::string& filepath,
                       const ParseOptions& options) {
  static SerializedRs274Parser parser;
  return parser.parse_file(filepath, options);
}

}  // namespace linuxcnc::server::gcode

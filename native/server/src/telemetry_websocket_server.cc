#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "linuxcnc/v1/websocket.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/gcode/parser.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"
#include "linuxcnc_grpc/position/telemetry.hpp"
#include "linuxcnc_grpc/program/workspace.hpp"
#include "linuxcnc_grpc/protobuf_gcode_mapping.hpp"
#include "linuxcnc_grpc/scope/controller.hpp"
#include "linuxcnc_grpc/scope/telemetry.hpp"

namespace linuxcnc::server {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using PlainStream = beast::tcp_stream;

constexpr auto kPositionDeliveryPeriod = std::chrono::milliseconds(50);
constexpr auto kWriteDeadline = std::chrono::seconds(5);
constexpr std::size_t kMaxSessions = 16;

linuxcnc::v1::FrameKind frame_kind(bool replacement) {
  return replacement ? linuxcnc::v1::FRAME_KIND_REPLACEMENT
                     : linuxcnc::v1::FRAME_KIND_DELTA;
}

std::string encode_position_frame(const PositionHistoryBatch& batch,
                                  bool replacement) {
  linuxcnc::v1::PositionHistoryFrame frame;
  frame.set_kind(frame_kind(replacement));
  frame.set_generation(batch.generation);
  frame.set_first_sequence(batch.first_sequence);
  frame.set_next_sequence(batch.next_sequence);
  frame.set_replacement_count(replacement ? 0 : batch.replace_count);
  for (const auto value : batch.packed) frame.add_values(value);
  return frame.SerializeAsString();
}

void encode_hal_scalar(const HalTelemetryValue& source,
                       linuxcnc::v1::HalScalar* target) {
  if (const auto* value = std::get_if<bool>(&source)) {
    target->set_type(linuxcnc::v1::HAL_TYPE_BIT);
    target->set_bit(*value);
  } else if (const auto* value = std::get_if<double>(&source)) {
    target->set_type(linuxcnc::v1::HAL_TYPE_FLOAT);
    target->set_float_value(*value);
  } else if (const auto* value = std::get_if<std::int32_t>(&source)) {
    target->set_type(linuxcnc::v1::HAL_TYPE_S32);
    target->set_s32(*value);
  } else if (const auto* value = std::get_if<std::uint32_t>(&source)) {
    target->set_type(linuxcnc::v1::HAL_TYPE_U32);
    target->set_u32(*value);
  } else if (const auto* value = std::get_if<std::int64_t>(&source)) {
    target->set_type(linuxcnc::v1::HAL_TYPE_S64);
    target->set_s64(*value);
  } else {
    target->set_type(linuxcnc::v1::HAL_TYPE_U64);
    target->set_u64(std::get<std::uint64_t>(source));
  }
}

std::string encode_hal_frame(const HalTelemetrySnapshot& snapshot,
                             bool replacement,
                             const std::vector<std::size_t>& changed) {
  linuxcnc::v1::HalValueFrame frame;
  frame.set_kind(frame_kind(replacement));
  frame.set_revision(snapshot.revision);
  frame.set_sequence(snapshot.sequence);
  const auto append = [&](std::size_t index) {
    auto* entry = frame.add_entries();
    entry->set_slot(snapshot.bindings.at(index).slot);
    if (snapshot.values.at(index))
      encode_hal_scalar(*snapshot.values[index], entry->mutable_value());
  };
  if (replacement) {
    for (std::size_t index = 0; index < snapshot.bindings.size(); ++index)
      append(index);
  } else {
    for (const auto index : changed) append(index);
  }
  return frame.SerializeAsString();
}

template <typename Source>
void encode_scope_channels(
    const Source& source,
    google::protobuf::RepeatedPtrField<linuxcnc::v1::PackedChannel>* target) {
  for (std::size_t index = 0; index < source.channels.size(); ++index) {
    auto* channel = target->Add();
    channel->set_index(static_cast<std::uint32_t>(index));
    channel->set_enabled(source.channels[index].has_value());
    if (const auto& values = source.channels[index]; values) {
      for (const auto value : *values) channel->add_values(value);
    }
  }
}

std::string encode_scope_frame(const ScopeFrame& source) {
  linuxcnc::v1::ScopeTelemetryFrame frame;
  if (source.kind == ScopeFrameKind::Capture) {
    const auto& capture = std::get<ScopeCapture>(source.payload);
    auto* encoded = frame.mutable_capture();
    encode_scope_channels(capture, encoded->mutable_channels());
    encoded->set_samples(
        static_cast<std::uint32_t>(std::max(0, capture.samples)));
    encoded->set_trigger_index(
        static_cast<std::uint32_t>(std::max(0, capture.trigger_index)));
    encoded->set_sample_period_ns(static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, capture.sample_period_ns)));
    encoded->set_generation(source.generation);
    encoded->set_skipped_frames(source.skipped_frames);
  } else {
    const auto& delta = std::get<ScopeCaptureDelta>(source.payload);
    auto* encoded = frame.mutable_roll();
    encode_scope_channels(delta, encoded->mutable_channels());
    encoded->set_samples(
        static_cast<std::uint32_t>(std::max(0, delta.samples)));
    encoded->set_capacity(
        static_cast<std::uint32_t>(std::max(0, delta.capacity)));
    encoded->set_sequence(delta.sequence);
    encoded->set_sample_period_ns(static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, delta.sample_period_ns)));
    encoded->set_reset(delta.reset);
    encoded->set_generation(source.generation);
    encoded->set_skipped_frames(source.skipped_frames);
  }
  return frame.SerializeAsString();
}

std::pair<std::string, std::string> split_endpoint(
    const std::string& endpoint) {
  const auto separator = endpoint.rfind(':');
  if (separator == std::string::npos) {
    throw std::invalid_argument("telemetry endpoint must be HOST:PORT");
  }
  auto host = endpoint.substr(0, separator);
  if (host.size() > 1 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  return {host, endpoint.substr(separator + 1)};
}

int hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::string> decode_query_value(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      decoded.push_back(' ');
    } else if (value[index] == '%') {
      if (index + 2 >= value.size()) return std::nullopt;
      const auto high = hex(value[index + 1]);
      const auto low = hex(value[index + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else {
      decoded.push_back(value[index]);
    }
  }
  return decoded;
}

std::optional<std::pair<std::string, std::string>> preview_parameters(
    std::string_view target) {
  constexpr std::string_view path = "/v1/program-preview?";
  if (target.rfind(path, 0) != 0) return std::nullopt;
  std::string workspace;
  std::string relative;
  auto query = target.substr(path.size());
  while (!query.empty()) {
    const auto ampersand = query.find('&');
    const auto item = query.substr(0, ampersand);
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    const auto name = item.substr(0, equals);
    const auto decoded = decode_query_value(item.substr(equals + 1));
    if (!decoded) return std::nullopt;
    if (name == "workspace_id") workspace = *decoded;
    if (name == "relative_path") relative = *decoded;
    if (ampersand == std::string_view::npos) break;
    query.remove_prefix(ampersand + 1);
  }
  if (workspace.empty() || relative.empty()) return std::nullopt;
  return std::pair{std::move(workspace), std::move(relative)};
}

class Session final : public std::enable_shared_from_this<Session> {
  struct RouteDependencies {
    std::shared_ptr<PositionTelemetry> position_telemetry;
    std::shared_ptr<HalValueTelemetry> hal_telemetry;
    std::shared_ptr<ScopeTelemetry> scope_telemetry;
    std::shared_ptr<ProgramWorkspaceStore> workspaces;
    BoundedExecutor* parser_worker;
    AdmissionCounter* preview_admission;
    std::filesystem::path ini_file;
    std::size_t batch_size;
  };

  struct PreviewFlow {
    std::stop_source stop_source;
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t outstanding_batches = 0;
  };

  struct PreviewMessage {
    std::string bytes;
    bool batch = false;
    bool terminal = false;
    bool progress = false;
  };

  struct PositionSession {
    PositionSession(const asio::any_io_executor& executor,
                    std::shared_ptr<PositionTelemetry> source)
        : delivery_timer(executor), telemetry(std::move(source)) {}

    asio::steady_timer delivery_timer;
    std::shared_ptr<PositionTelemetry> telemetry;
    PositionTelemetry::Subscription subscription;
    std::uint64_t cursor = 0;
    std::uint64_t generation = 0;
    std::uint64_t write_cursor = 0;
    std::uint64_t write_generation = 0;
    std::chrono::steady_clock::time_point next_delivery;
    std::atomic<bool> wake_pending{false};
    bool delivery_scheduled = false;
  };

  struct HalSession {
    HalSession(std::shared_ptr<HalValueTelemetry> source,
               std::string subscription_id)
        : telemetry(std::move(source)), id(std::move(subscription_id)) {}

    std::shared_ptr<HalValueTelemetry> telemetry;
    HalValueTelemetry::Subscription subscription;
    std::optional<HalTelemetrySnapshot> write_snapshot;
    std::vector<std::optional<HalTelemetryValue>> values;
    std::string id;
    std::uint64_t revision = 0;
    bool dirty = false;
  };

  struct ScopeSession {
    ScopeSession(std::shared_ptr<ScopeTelemetry> source, std::string token)
        : telemetry(std::move(source)), token(std::move(token)) {}

    std::shared_ptr<ScopeTelemetry> telemetry;
    ScopeTelemetry::Subscription subscription;
    std::string token;
    std::uint64_t generation = 0;
  };

  struct PreviewSession {
    PreviewSession(std::shared_ptr<ProgramWorkspaceStore> workspace_store,
                   BoundedExecutor& worker, AdmissionCounter& admission,
                   std::filesystem::path ini, std::size_t size,
                   std::string workspace, std::string relative_path)
        : workspaces(std::move(workspace_store)),
          parser_worker(worker),
          admission(admission),
          ini_file(std::move(ini)),
          batch_size(size),
          workspace_id(std::move(workspace)),
          relative_path(std::move(relative_path)) {}

    std::shared_ptr<ProgramWorkspaceStore> workspaces;
    BoundedExecutor& parser_worker;
    AdmissionCounter& admission;
    std::filesystem::path ini_file;
    std::size_t batch_size;
    std::string workspace_id;
    std::string relative_path;
    std::shared_ptr<PreviewFlow> flow = std::make_shared<PreviewFlow>();
    std::deque<PreviewMessage> queue;
    bool active_batch = false;
    bool active_terminal = false;
    bool admitted = false;
  };

 public:
  Session(PlainStream stream,
          std::shared_ptr<PositionTelemetry> position_telemetry,
          std::shared_ptr<HalValueTelemetry> hal_telemetry,
          std::shared_ptr<ScopeTelemetry> scope_telemetry,
          std::shared_ptr<ProgramWorkspaceStore> workspaces,
          BoundedExecutor& parser_worker, AdmissionCounter& preview_admission,
          std::filesystem::path ini_file, std::size_t batch_size,
          std::function<void()> release)
      : websocket_(std::move(stream)),
        write_deadline_(websocket_.get_executor()),
        dependencies_(RouteDependencies{
            std::move(position_telemetry), std::move(hal_telemetry),
            std::move(scope_telemetry), std::move(workspaces), &parser_worker,
            &preview_admission, std::move(ini_file), batch_size}),
        release_(std::move(release)) {}

  void run() { read_upgrade(); }

  void stop() {
    if (preview_) {
      preview_->flow->stop_source.request_stop();
      preview_->flow->condition.notify_all();
    }
    abort_socket();
    fail();
  }

 private:
  void read_upgrade() {
    http::async_read(websocket_.next_layer(), read_buffer_, request_,
                     beast::bind_front_handler(&Session::on_upgrade_request,
                                               this->shared_from_this()));
  }

  void on_upgrade_request(beast::error_code error, std::size_t) {
    if (error || !websocket::is_upgrade(request_)) return fail();
    if (!dependencies_) return fail();
    auto& dependencies = *dependencies_;
    const std::string target(request_.target());
    if (target == "/v1/position-history") {
      position_ = std::make_unique<PositionSession>(
          websocket_.get_executor(),
          std::move(dependencies.position_telemetry));
    } else if (const auto parameters = preview_parameters(target)) {
      preview_ = std::make_unique<PreviewSession>(
          std::move(dependencies.workspaces), *dependencies.parser_worker,
          *dependencies.preview_admission, std::move(dependencies.ini_file),
          dependencies.batch_size, parameters->first, parameters->second);
    } else {
      constexpr std::string_view hal_prefix = "/v1/hal-values/";
      constexpr std::string_view scope_prefix = "/v1/scope/";
      if (target.rfind(hal_prefix, 0) == 0) {
        auto claimed =
            dependencies.hal_telemetry->claim(target.substr(hal_prefix.size()));
        if (!claimed) return fail();
        hal_ = std::make_unique<HalSession>(
            std::move(dependencies.hal_telemetry), std::move(*claimed));
      } else if (target.rfind(scope_prefix, 0) == 0) {
        auto claimed = dependencies.scope_telemetry->claim(
            target.substr(scope_prefix.size()));
        if (!claimed) return fail();
        scope_ = std::make_unique<ScopeSession>(
            std::move(dependencies.scope_telemetry), std::move(*claimed));
      } else {
        return fail();
      }
    }
    dependencies_.reset();
    websocket_.binary(true);
    websocket_.read_message_max(1024);
    websocket_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));
    websocket_.async_accept(
        request_, beast::bind_front_handler(&Session::on_accept,
                                            this->shared_from_this()));
  }

  void on_accept(beast::error_code error) {
    if (error) return fail();
    const std::weak_ptr<Session> weak = this->shared_from_this();
    if (position_) {
      position_->subscription =
          position_->telemetry->subscribe([weak](const std::uint64_t&) {
            if (const auto self = weak.lock()) self->queue_position_wake();
          });
      send_next(true);
      position_->next_delivery =
          std::chrono::steady_clock::now() + kPositionDeliveryPeriod;
      schedule_position_delivery();
    } else if (hal_) {
      const auto callback = [weak](const std::uint64_t&) {
        if (const auto self = weak.lock()) {
          asio::post(self->websocket_.get_executor(), [weak] {
            if (const auto session = weak.lock()) session->wake_hal();
          });
        }
      };
      hal_->subscription = hal_->telemetry->subscribe(hal_->id, callback);
      send_hal();
    } else if (scope_) {
      scope_->subscription = scope_->telemetry->subscribe(
          scope_->token, [weak](const std::uint64_t&) {
            if (const auto self = weak.lock()) {
              asio::post(self->websocket_.get_executor(), [weak] {
                if (const auto session = weak.lock()) session->wake_scope();
              });
            }
          });
      wake_scope();
    } else {
      start_preview();
    }
    read_application_data();
  }

  void queue_position_wake() {
    if (position_) position_->wake_pending = true;
  }

  void schedule_position_delivery() {
    if (closed_ || closing_ || !position_ || position_->delivery_scheduled)
      return;
    position_->delivery_scheduled = true;
    position_->delivery_timer.expires_at(position_->next_delivery);
    position_->delivery_timer.async_wait(beast::bind_front_handler(
        &Session::on_position_delivery, this->shared_from_this()));
  }

  void on_position_delivery(beast::error_code error) {
    if (!position_) return;
    position_->delivery_scheduled = false;
    if (error == asio::error::operation_aborted || closed_ || closing_) return;
    if (error) return fail();
    const auto now = std::chrono::steady_clock::now();
    do {
      position_->next_delivery += kPositionDeliveryPeriod;
    } while (position_->next_delivery <= now);
    schedule_position_delivery();
    if (!writing_ && position_->wake_pending.exchange(false)) send_next(false);
  }

  void wake_hal() {
    if (closed_) return;
    if (writing_) {
      hal_->dirty = true;
      return;
    }
    send_hal();
  }

  void send_next(bool initial) {
    auto& state = *position_;
    PositionHistoryBatch batch =
        initial ? state.telemetry->snapshot()
                : state.telemetry->since(state.cursor, state.generation);
    if (!initial && !batch.reset && batch.packed.empty()) return;
    const bool replacement = initial || batch.reset;
    state.write_cursor = batch.next_sequence;
    state.write_generation = batch.generation;
    writing_ = true;
    write_frame(encode_position_frame(batch, replacement));
  }

  void send_hal() {
    auto& state = *hal_;
    const auto snapshot = state.telemetry->snapshot(state.id);
    if (!snapshot) return fail();
    if (!snapshot->sampled) return;
    const bool replacement = state.revision != snapshot->revision;
    std::vector<std::size_t> changed;
    if (!replacement) {
      for (std::size_t index = 0; index < snapshot->values.size(); ++index) {
        if (index >= state.values.size() ||
            snapshot->values[index] != state.values[index])
          changed.push_back(index);
      }
      if (changed.empty()) return;
    }
    state.write_snapshot = *snapshot;
    writing_ = true;
    write_frame(encode_hal_frame(*snapshot, replacement, changed));
  }

  void wake_scope() {
    if (closed_ || writing_) return;
    auto& state = *scope_;
    const auto frame = state.telemetry->next(state.token);
    if (!frame) return;
    state.generation = frame->generation;
    writing_ = true;
    write_frame(encode_scope_frame(*frame));
  }

  void write_frame(std::string frame) {
    write_frame_ = std::move(frame);
    write_deadline_.expires_after(kWriteDeadline);
    write_deadline_.async_wait(
        [self = this->shared_from_this()](beast::error_code error) {
          if (!error) {
            self->abort_socket();
            self->fail();
          }
        });
    websocket_.async_write(asio::buffer(write_frame_),
                           beast::bind_front_handler(&Session::on_write,
                                                     this->shared_from_this()));
  }

  void on_write(beast::error_code error, std::size_t) {
    write_deadline_.cancel();
    writing_ = false;
    if (error) return fail();
    if (preview_) {
      if (preview_->active_batch) {
        std::lock_guard lock(preview_->flow->mutex);
        --preview_->flow->outstanding_batches;
        preview_->flow->condition.notify_all();
      }
      const bool terminal = preview_->active_terminal;
      preview_->active_batch = false;
      preview_->active_terminal = false;
      write_frame_.clear();
      if (closing_) return close_policy_violation();
      if (terminal) return close_normal();
      pump_preview();
      return;
    }
    if (scope_) {
      write_frame_.clear();
      if (closing_) return close_policy_violation();
      const auto next =
          scope_->telemetry->acknowledge(scope_->token, scope_->generation);
      if (next) {
        scope_->generation = next->generation;
        writing_ = true;
        write_frame(encode_scope_frame(*next));
      }
      return;
    }
    if (position_) {
      position_->cursor = position_->write_cursor;
      position_->generation = position_->write_generation;
    }
    if (hal_ && hal_->write_snapshot) {
      hal_->revision = hal_->write_snapshot->revision;
      hal_->values = hal_->write_snapshot->values;
      hal_->write_snapshot.reset();
    }
    write_frame_.clear();
    if (closing_) return close_policy_violation();
    if (hal_ && hal_->dirty) {
      hal_->dirty = false;
      send_hal();
    }
  }

  void read_application_data() {
    websocket_.async_read(
        read_buffer_,
        beast::bind_front_handler(&Session::on_read, this->shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
    if (error == websocket::error::closed) return fail();
    if (error) return fail();
    closing_ = true;
    if (!writing_) close_policy_violation();
  }

  void close_policy_violation() {
    websocket::close_reason reason(websocket::close_code::policy_error);
    reason.reason = "telemetry is server-to-client only";
    websocket_.async_close(reason, [self = this->shared_from_this()](
                                       beast::error_code) { self->fail(); });
  }

  void enqueue_preview(std::string bytes, bool batch, bool terminal,
                       bool progress = false) {
    const std::weak_ptr<Session> weak = this->shared_from_this();
    asio::post(websocket_.get_executor(), [weak, bytes = std::move(bytes),
                                           batch, terminal,
                                           progress]() mutable {
      const auto self = weak.lock();
      if (!self || self->closed_) return;
      auto& preview = *self->preview_;
      if (progress && !preview.queue.empty() && preview.queue.back().progress) {
        preview.queue.back().bytes = std::move(bytes);
      } else {
        preview.queue.push_back({std::move(bytes), batch, terminal, progress});
      }
      self->pump_preview();
    });
  }

  void start_preview() {
    auto& preview = *preview_;
    if (!preview.admission.acquire()) return close_try_again_later();
    preview.admitted = true;
    const auto flow = preview.flow;
    const auto store = preview.workspaces;
    const auto workspace_id = preview.workspace_id;
    const auto relative_path = preview.relative_path;
    const auto ini_file = preview.ini_file;
    const auto batch_size = preview.batch_size;
    const std::weak_ptr<Session> weak = this->shared_from_this();
    if (!preview.parser_worker.submit([flow, store, workspace_id, relative_path,
                                       ini_file, batch_size, weak] {
          static_cast<void>(batch_size);
          const auto send =
              [weak](const linuxcnc::v1::ProgramPreviewEvent& event, bool batch,
                     bool terminal, bool progress = false) {
                if (const auto self = weak.lock())
                  self->enqueue_preview(event.SerializeAsString(), batch,
                                        terminal, progress);
              };
#ifdef LINUXCNC_GRPC_HAS_RS274
          std::filesystem::path source;
          const bool leased =
              store->pin_entry(workspace_id, relative_path, &source);
          if (!leased) {
            linuxcnc::v1::ProgramPreviewEvent event;
            auto* error = event.mutable_error();
            error->set_code(
                linuxcnc::v1::PROGRAM_PREVIEW_ERROR_CODE_INVALID_ENTRY);
            error->set_message("program workspace entry is missing or unsafe");
            send(event, false, true);
            return;
          }
          struct Lease {
            std::shared_ptr<ProgramWorkspaceStore> store;
            std::string workspace;
            ~Lease() { store->unpin_entry(workspace); }
          } lease{store, workspace_id};
          try {
            gcode::ParseOptions options;
            options.ini_path = ini_file.string();
            options.program_prefix = (store->root() / workspace_id).string();
            options.batch_size = batch_size;
            options.stop_token = flow->stop_source.get_token();
            options.on_progress = [send](const gcode::ParseProgress& progress) {
              linuxcnc::v1::ProgramPreviewEvent event;
              auto* encoded = event.mutable_progress();
              encoded->set_bytes_read(progress.bytesRead);
              encoded->set_total_bytes(progress.totalBytes);
              encoded->set_percent(static_cast<std::uint32_t>(
                  std::clamp(progress.percent, 0.0, 100.0)));
              encoded->set_operation_count(progress.operationCount);
              send(event, false, false, true);
            };
            options.on_batch = [flow, send](gcode::OperationBatch&& batch) {
              if (batch.empty()) return;
              const auto send_batch =
                  [flow, send](linuxcnc::v1::ProgramPreviewEvent&& event) {
                    std::unique_lock lock(flow->mutex);
                    flow->condition.wait(lock, [flow] {
                      return flow->stop_source.stop_requested() ||
                             flow->outstanding_batches < 2;
                    });
                    if (flow->stop_source.stop_requested()) return false;
                    ++flow->outstanding_batches;
                    lock.unlock();
                    send(event, true, false);
                    return true;
                  };
              constexpr std::size_t max_preview_frame_bytes =
                  static_cast<std::size_t>(4U) * 1024U * 1024U;
              linuxcnc::v1::ProgramPreviewEvent event;
              for (const auto& operation : batch) {
                encode_gcode_operation(operation,
                                       event.mutable_batch()->add_operations());
                if (event.ByteSizeLong() <= max_preview_frame_bytes) continue;
                event.mutable_batch()->mutable_operations()->RemoveLast();
                if (event.batch().operations().empty())
                  throw std::runtime_error(
                      "G-code operation exceeds preview frame byte limit");
                if (!send_batch(std::move(event))) return;
                event.Clear();
                encode_gcode_operation(operation,
                                       event.mutable_batch()->add_operations());
                if (event.ByteSizeLong() > max_preview_frame_bytes)
                  throw std::runtime_error(
                      "G-code operation exceeds preview frame byte limit");
              }
              if (!event.batch().operations().empty())
                (void)send_batch(std::move(event));
            };
            const auto result = gcode::parse_file(source.string(), options);
            if (result.cancelled) return;
            linuxcnc::v1::ProgramPreviewEvent event;
            auto* summary = event.mutable_summary();
            encode_gcode_extents(result.extents, summary->mutable_extents());
            summary->set_operation_count(result.operationCount);
            send(event, false, true);
          } catch (const gcode::ParseError& exception) {
            linuxcnc::v1::ProgramPreviewEvent event;
            auto* error = event.mutable_error();
            switch (exception.code()) {
              case gcode::ParseErrorCode::InvalidEntry:
                error->set_code(
                    linuxcnc::v1::PROGRAM_PREVIEW_ERROR_CODE_INVALID_ENTRY);
                break;
              case gcode::ParseErrorCode::Interpreter:
                error->set_code(
                    linuxcnc::v1::PROGRAM_PREVIEW_ERROR_CODE_INTERPRETER);
                break;
              case gcode::ParseErrorCode::Internal:
                error->set_code(
                    linuxcnc::v1::PROGRAM_PREVIEW_ERROR_CODE_INTERNAL);
                break;
            }
            error->set_message(exception.what());
            if (exception.line_number())
              error->set_line_number(*exception.line_number());
            send(event, false, true);
          } catch (const std::exception& exception) {
            linuxcnc::v1::ProgramPreviewEvent event;
            auto* error = event.mutable_error();
            error->set_code(linuxcnc::v1::PROGRAM_PREVIEW_ERROR_CODE_INTERNAL);
            error->set_message(exception.what());
            send(event, false, true);
          }
#else
          if (const auto self = weak.lock()) {
            asio::post(self->websocket_.get_executor(), [weak] {
              if (const auto session = weak.lock())
                session->close_try_again_later();
            });
          }
#endif
        })) {
      close_try_again_later();
    }
  }

  void pump_preview() {
    auto& preview = *preview_;
    if (writing_ || preview.queue.empty() || closed_) return;
    auto message = std::move(preview.queue.front());
    preview.queue.pop_front();
    preview.active_batch = message.batch;
    preview.active_terminal = message.terminal;
    writing_ = true;
    write_frame(std::move(message.bytes));
  }

  void close_normal() {
    websocket_.async_close(
        websocket::close_code::normal,
        [self = this->shared_from_this()](beast::error_code) { self->fail(); });
  }

  void close_try_again_later() {
    websocket::close_reason reason(websocket::close_code::try_again_later);
    reason.reason = "preview capacity unavailable";
    websocket_.async_close(reason, [self = this->shared_from_this()](
                                       beast::error_code) { self->fail(); });
  }

  void fail() {
    if (closed_) return;
    closed_ = true;
    if (preview_) {
      preview_->flow->stop_source.request_stop();
      preview_->flow->condition.notify_all();
    }
    if (position_) position_->delivery_timer.cancel();
    write_deadline_.cancel();
    if (position_) position_->subscription.reset();
    if (hal_) {
      hal_->subscription.reset();
      hal_->telemetry->detach(hal_->id);
      hal_->id.clear();
    }
    if (scope_) {
      scope_->subscription.reset();
      scope_->telemetry->release(scope_->token);
      scope_->token.clear();
    }
    if (preview_ && preview_->admitted) {
      preview_->admitted = false;
      preview_->admission.release();
    }
    if (release_) {
      release_();
      release_ = {};
    }
    write_frame_.clear();
    if (preview_) preview_->queue.clear();
    abort_socket();
  }

  void abort_socket() {
    beast::error_code ignored;
    // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort teardown
    (void)beast::get_lowest_layer(websocket_)
        .socket()
        .shutdown(tcp::socket::shutdown_both, ignored);
    // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort teardown
    (void)beast::get_lowest_layer(websocket_).socket().close(ignored);
  }

  websocket::stream<PlainStream> websocket_;
  asio::steady_timer write_deadline_;
  std::optional<RouteDependencies> dependencies_;
  std::unique_ptr<PositionSession> position_;
  std::unique_ptr<HalSession> hal_;
  std::unique_ptr<ScopeSession> scope_;
  std::unique_ptr<PreviewSession> preview_;
  std::function<void()> release_;
  beast::flat_buffer read_buffer_;
  http::request<http::string_body> request_;
  std::string write_frame_;
  bool writing_ = false;
  bool closing_ = false;
  bool closed_ = false;
};

}  // namespace

class TelemetryWebSocketServer::Impl {
 public:
  Impl(const DaemonConfig& config,
       std::shared_ptr<PositionTelemetry> position_telemetry,
       std::shared_ptr<HalValueTelemetry> hal_telemetry,
       std::shared_ptr<ScopeTelemetry> scope_telemetry,
       std::shared_ptr<ProgramWorkspaceStore> workspaces,
       BoundedExecutor& parser_worker, AdmissionCounter& preview_admission)
      : position_telemetry_(std::move(position_telemetry)),
        hal_telemetry_(std::move(hal_telemetry)),
        scope_telemetry_(std::move(scope_telemetry)),
        workspaces_(std::move(workspaces)),
        parser_worker_(parser_worker),
        preview_admission_(preview_admission),
        ini_file_(config.ini_file),
        batch_size_(config.gcode_batch_size),
        acceptor_(io_) {
    const auto [host, port] = split_endpoint(config.telemetry_endpoint);
    tcp::resolver resolver(io_);
    const auto resolved = resolver.resolve(host, port);
    if (resolved.empty())
      throw std::runtime_error("cannot resolve telemetry endpoint");
    const auto endpoint = resolved.begin()->endpoint();
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
    accept();
    thread_ = std::thread([this] { io_.run(); });
  }

  ~Impl() { stop(); }

  void stop() {
    if (stopped_.exchange(true)) return;
    asio::post(io_, [this] {
      beast::error_code ignored;
      // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort teardown
      (void)acceptor_.cancel(ignored);
      // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort teardown
      (void)acceptor_.close(ignored);
      for (auto& weak : sessions_) {
        if (const auto session = weak.lock()) session->stop();
      }
      sessions_.clear();
    });
    if (thread_.joinable()) thread_.join();
  }

 private:
  void accept() {
    acceptor_.async_accept([this](beast::error_code error, tcp::socket socket) {
      if (!error) {
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                           [](const auto& weak) { return weak.expired(); }),
            sessions_.end());
        if (active_sessions_.fetch_add(1) >= kMaxSessions) {
          active_sessions_.fetch_sub(1);
          beast::error_code ignored;
          // NOLINTNEXTLINE(bugprone-unused-return-value): rejected connection
          (void)socket.close(ignored);
        } else {
          auto release = [this] { active_sessions_.fetch_sub(1); };
          auto session = std::make_shared<Session>(
              PlainStream(std::move(socket)), position_telemetry_,
              hal_telemetry_, scope_telemetry_, workspaces_, parser_worker_,
              preview_admission_, ini_file_, batch_size_, std::move(release));
          sessions_.push_back(session);
          session->run();
        }
      }
      if (!stopped_) accept();
    });
  }

  asio::io_context io_{1};
  std::shared_ptr<PositionTelemetry> position_telemetry_;
  std::shared_ptr<HalValueTelemetry> hal_telemetry_;
  std::shared_ptr<ScopeTelemetry> scope_telemetry_;
  std::shared_ptr<ProgramWorkspaceStore> workspaces_;
  BoundedExecutor& parser_worker_;
  AdmissionCounter& preview_admission_;
  const std::filesystem::path ini_file_;
  const std::size_t batch_size_;
  tcp::acceptor acceptor_;
  std::atomic<bool> stopped_{false};
  std::atomic<std::size_t> active_sessions_{0};
  std::vector<std::weak_ptr<Session>> sessions_;
  std::thread thread_;
};

TelemetryWebSocketServer::TelemetryWebSocketServer(
    const DaemonConfig& config,
    std::shared_ptr<PositionTelemetry> position_telemetry,
    std::shared_ptr<HalValueTelemetry> hal_telemetry,
    std::shared_ptr<ScopeTelemetry> scope_telemetry,
    std::shared_ptr<ProgramWorkspaceStore> workspaces,
    BoundedExecutor& parser_worker, AdmissionCounter& preview_admission)
    : impl_(std::make_unique<Impl>(
          config, std::move(position_telemetry), std::move(hal_telemetry),
          std::move(scope_telemetry), std::move(workspaces), parser_worker,
          preview_admission)) {}

TelemetryWebSocketServer::~TelemetryWebSocketServer() = default;

void TelemetryWebSocketServer::stop() { impl_->stop(); }

}  // namespace linuxcnc::server

#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_transport/credentials.hpp"
#include "telemetry_session.hpp"
#include "telemetry_stream.hpp"

namespace linuxcnc::server {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using detail::TelemetryStream;
constexpr std::size_t kMaxSessions = 16;

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

}  // namespace

class TelemetryWebSocketServer::Impl {
 public:
  Impl(const DaemonConfig& config,
       std::shared_ptr<PositionTelemetry> position_telemetry,
       std::shared_ptr<HalValueTelemetry> hal_telemetry,
       std::shared_ptr<ScopeTelemetry> scope_telemetry,
       std::shared_ptr<ProgramWorkspaceStore> workspaces,
       BoundedExecutor& parser_worker, AdmissionCounter& preview_admission,
       const std::shared_ptr<transport::ServerIdentity>& identity)
      : position_telemetry_(std::move(position_telemetry)),
        hal_telemetry_(std::move(hal_telemetry)),
        scope_telemetry_(std::move(scope_telemetry)),
        workspaces_(std::move(workspaces)),
        parser_worker_(parser_worker),
        preview_admission_(preview_admission),
        config_(config),
        acceptor_(io_) {
    if (config.tls != static_cast<bool>(identity))
      throw std::runtime_error(
          "telemetry TLS identity does not match listener policy");
    if (identity && identity->require_client_certificate() != config.mtls)
      throw std::runtime_error("telemetry client certificate policy mismatch");
    if (identity) {
      tls_context_ =
          std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
      identity->Configure(tls_context_->native_handle());
    }
    // Isolate potentially blocking provider callbacks from accept/control I/O.
    // Each session's strand serializes its I/O, timers, and shutdown edge. A
    // handshake waiting for an external signer holds its worker, so one worker
    // per admitted session keeps streaming sessions running while others wait.
    session_workers_ = std::make_unique<asio::thread_pool>(kMaxSessions);
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
        if (const auto session = weak.lock())
          asio::post(session->get_executor(), [session] { session->stop(); });
      }
      sessions_.clear();
    });
    if (thread_.joinable()) thread_.join();
    // Drain cancelled handlers before the SSL context/provider can be released.
    session_workers_->join();
  }

 private:
  void accept() {
    acceptor_.async_accept(
        asio::make_strand(*session_workers_),
        [this](beast::error_code error, tcp::socket socket) {
          if (!error && !stopped_) {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(),
                               [](const auto& weak) { return weak.expired(); }),
                sessions_.end());
            if (active_sessions_.fetch_add(1) >= kMaxSessions) {
              active_sessions_.fetch_sub(1);
              beast::error_code ignored;
              // NOLINTNEXTLINE(bugprone-unused-return-value)
              (void)socket.close(ignored);
            } else {
              start_session(std::move(socket));
            }
          }
          if (!stopped_) accept();
        });
  }

  // Runs on the accept thread with a slot already taken. A session that cannot
  // be created (for example SSL_new failing) frees its slot and drops only
  // this connection; the accept loop keeps running.
  void start_session(tcp::socket socket) {
    try {
      auto release = [this] { active_sessions_.fetch_sub(1); };
      auto session = detail::make_telemetry_session(
          TelemetryStream(std::move(socket), tls_context_.get()), config_,
          position_telemetry_, hal_telemetry_, scope_telemetry_, workspaces_,
          parser_worker_, preview_admission_, std::move(release));
      sessions_.push_back(session);
      asio::post(session->get_executor(), [session] { session->run(); });
    } catch (const std::exception&) {
      active_sessions_.fetch_sub(1);
    }
  }

  asio::io_context io_{1};
  std::unique_ptr<asio::ssl::context> tls_context_;
  std::unique_ptr<asio::thread_pool> session_workers_;
  std::shared_ptr<PositionTelemetry> position_telemetry_;
  std::shared_ptr<HalValueTelemetry> hal_telemetry_;
  std::shared_ptr<ScopeTelemetry> scope_telemetry_;
  std::shared_ptr<ProgramWorkspaceStore> workspaces_;
  BoundedExecutor& parser_worker_;
  AdmissionCounter& preview_admission_;
  const DaemonConfig config_;
  tcp::acceptor acceptor_;
  std::atomic<bool> stopped_{false};
  std::atomic<std::size_t> active_sessions_{0};
  std::vector<std::weak_ptr<detail::TelemetrySession>> sessions_;
  std::thread thread_;
};

TelemetryWebSocketServer::TelemetryWebSocketServer(
    const DaemonConfig& config,
    std::shared_ptr<PositionTelemetry> position_telemetry,
    std::shared_ptr<HalValueTelemetry> hal_telemetry,
    std::shared_ptr<ScopeTelemetry> scope_telemetry,
    std::shared_ptr<ProgramWorkspaceStore> workspaces,
    BoundedExecutor& parser_worker, AdmissionCounter& preview_admission,
    const std::shared_ptr<transport::ServerIdentity>& identity)
    : impl_(std::make_unique<Impl>(
          config, std::move(position_telemetry), std::move(hal_telemetry),
          std::move(scope_telemetry), std::move(workspaces), parser_worker,
          preview_admission, identity)) {}

TelemetryWebSocketServer::~TelemetryWebSocketServer() = default;

void TelemetryWebSocketServer::stop() { impl_->stop(); }

}  // namespace linuxcnc::server

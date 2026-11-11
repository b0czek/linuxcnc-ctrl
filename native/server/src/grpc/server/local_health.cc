#include "grpc/server/local_health.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/linuxcnc/machine_health.hpp"

namespace linuxcnc::server::detail {
namespace {

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() { reset(); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return fd_; }
  int release() { return std::exchange(fd_, -1); }
  void reset(int fd = -1) {
    if (fd_ >= 0) {
      // NOLINTNEXTLINE(bugprone-unused-return-value): closing an owned fd
      (void)close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_;
};

bool same_inode(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

class HealthEndpoint {
 public:
  explicit HealthEndpoint(const std::string& endpoint) {
    std::string error;
    if (endpoint.empty() || !validate_health_endpoint(endpoint, &error))
      throw std::runtime_error("invalid local health endpoint: " + error);
    path_ = endpoint.substr(5);
    name_ = path_.filename().string();
    directory_.reset(open(path_.parent_path().c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat directory{};
    if (directory_.get() < 0 || fstat(directory_.get(), &directory) != 0 ||
        !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 0077) != 0)
      throw std::runtime_error(
          "health socket parent must be a private directory owned by the "
          "daemon user");

    const auto value = path_.string();
    address_.sun_family = AF_UNIX;
    std::copy(value.begin(), value.end(), address_.sun_path);
    reclaim_stale_socket();
  }

  ~HealthEndpoint() {
    struct stat current{};
    if (owned_ && inspect(&current) == 0 && S_ISSOCK(current.st_mode) &&
        same_inode(socket_, current)) {
      // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort owned cleanup
      (void)unlinkat(directory_.get(), name_.c_str(), 0);
    }
  }

  void listen() {
    listener_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    // bind never removes an occupied path, so a live socket at this path is
    // never replaced.
    if (listener_.get() < 0 ||
        bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address_),
             sizeof(address_)) != 0)
      throw std::runtime_error("cannot bind local health endpoint");
    if (inspect(&socket_) != 0 || !S_ISSOCK(socket_.st_mode))
      throw std::runtime_error("cannot identify local health socket");
    owned_ = true;
    if (::listen(listener_.get(), 16) != 0)
      throw std::runtime_error("cannot listen on local health endpoint");
  }

  int fd() const { return listener_.get(); }

  void stop_accepting() const {
    // Wake accept without closing/reusing the fd while its thread is running.
    // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort socket shutdown
    (void)::shutdown(listener_.get(), SHUT_RDWR);
  }

 private:
  int inspect(struct stat* result) const {
    return fstatat(directory_.get(), name_.c_str(), result,
                   AT_SYMLINK_NOFOLLOW);
  }

  // The parent is private to the daemon user, so only a previous run of this
  // daemon can have left a socket here. Remove it once nothing accepts on it.
  void reclaim_stale_socket() {
    struct stat previous{};
    if (inspect(&previous) != 0) {
      if (errno == ENOENT) return;
      throw std::runtime_error("cannot inspect local health socket path");
    }
    if (!S_ISSOCK(previous.st_mode))
      throw std::runtime_error("health socket path is not a socket");
    FileDescriptor probe(
        socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (probe.get() < 0 ||
        connect(probe.get(), reinterpret_cast<const sockaddr*>(&address_),
                sizeof(address_)) == 0 ||
        errno != ECONNREFUSED)
      throw std::runtime_error(
          "health socket is live or cannot be proven stale");
    if (unlinkat(directory_.get(), name_.c_str(), 0) != 0)
      throw std::runtime_error("cannot remove stale local health socket");
  }

  std::filesystem::path path_;
  std::string name_;
  FileDescriptor directory_;
  FileDescriptor listener_;
  sockaddr_un address_{};
  struct stat socket_{};
  bool owned_{false};
};

constexpr std::size_t kMaxRequestBytes = 1024;
constexpr auto kClientTimeout = std::chrono::seconds(1);

void set_timeouts(int fd) {
  timeval timeout{};
  timeout.tv_sec =
      std::chrono::duration_cast<std::chrono::seconds>(kClientTimeout).count();
  // NOLINTNEXTLINE(bugprone-unused-return-value): reads stay bounded by size
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort send bound
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

// Reads the request head. Returns its request line, or an empty string for a
// malformed, oversized or timed-out request.
std::string read_request_line(int fd) {
  std::string request;
  char buffer[256];
  while (request.find("\r\n\r\n") == std::string::npos) {
    if (request.size() >= kMaxRequestBytes) return {};
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return {};
    request.append(buffer, static_cast<std::size_t>(count));
  }
  return request.substr(0, request.find("\r\n"));
}

void send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto count =
        send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return;
    sent += static_cast<std::size_t>(count);
  }
}

std::string response(const char* status, const std::string& body) {
  return std::string("HTTP/1.1 ") + status +
         "\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) +
         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

class LocalHealthServer::Impl {
 public:
  Impl(const std::string& endpoint,
       std::shared_ptr<const MachineHealthCache> machine)
      : endpoint_(endpoint), machine_(std::move(machine)) {
    endpoint_.listen();
    serving_.store(true, std::memory_order_release);
    accept_thread_ = std::thread([this] { accept_connections(); });
  }

  ~Impl() { shutdown(); }

  void shutdown() {
    serving_.store(false, std::memory_order_release);
    if (stopped_.exchange(true)) return;
    endpoint_.stop_accepting();
    if (accept_thread_.joinable()) accept_thread_.join();
  }

 private:
  // Requests are answered one at a time; each is bounded by size and by the
  // client socket timeouts, so a stalled client delays others by at most
  // about a second.
  void accept_connections() {
    while (!stopped_.load()) {
      FileDescriptor client(
          accept4(endpoint_.fd(), nullptr, nullptr, SOCK_CLOEXEC));
      if (client.get() < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
            errno == ENOMEM) {
          // Descriptor or memory exhaustion is transient; keep serving.
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        serving_.store(false, std::memory_order_release);
        return;
      }
      if (stopped_.load()) return;
      serve(client.get());
    }
  }

  void serve(int fd) const {
    set_timeouts(fd);
    const auto line = read_request_line(fd);
    if (line.empty()) {
      send_all(fd, response("400 Bad Request", R"({"error":"bad request"})"));
      return;
    }
    const auto method_end = line.find(' ');
    const auto method = line.substr(0, method_end);
    const auto target_end = line.find(' ', method_end + 1);
    const auto target =
        method_end == std::string::npos
            ? std::string{}
            : line.substr(method_end + 1, target_end - method_end - 1);
    if (target != "/health") {
      send_all(fd, response("404 Not Found", R"({"error":"not found"})"));
      return;
    }
    if (method != "GET") {
      send_all(fd, response("405 Method Not Allowed",
                            R"({"error":"method not allowed"})"));
      return;
    }
    const bool serving = serving_.load(std::memory_order_acquire);
    const auto machine = machine_ ? machine_->read() : MachineHealth{};
    send_all(fd, response(serving ? "200 OK" : "503 Service Unavailable",
                          format_health_json(serving, machine)));
  }

  HealthEndpoint endpoint_;
  std::shared_ptr<const MachineHealthCache> machine_;
  std::atomic<bool> serving_{false};
  std::atomic<bool> stopped_{false};
  std::thread accept_thread_;
};

LocalHealthServer::LocalHealthServer(
    const std::string& endpoint,
    std::shared_ptr<const MachineHealthCache> machine)
    : impl_(std::make_unique<Impl>(endpoint, std::move(machine))) {}
LocalHealthServer::~LocalHealthServer() = default;
void LocalHealthServer::shutdown() { impl_->shutdown(); }

}  // namespace linuxcnc::server::detail

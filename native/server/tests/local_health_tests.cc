#include <grpcpp/grpcpp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "grpc/server/local_health.hpp"
#include "grpc/server/runtime.hpp"
#include "grpc/server/service.hpp"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/linuxcnc/machine_health.hpp"
#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"
#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class PublicMachine final : public linuxcnc::v1::MachineService::Service {};

sockaddr_un unix_address(const std::filesystem::path& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto value = path.string();
  std::copy(value.begin(), value.end(), address.sun_path);
  return address;
}

// Sends one raw HTTP request and returns the whole reply, or an empty string
// when the endpoint cannot be reached.
std::string http_exchange(const std::filesystem::path& path,
                          const std::string& request) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  require(fd >= 0, "cannot create test client socket");
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const auto address = unix_address(path);
  std::string reply;
  if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) == 0 &&
      send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
          static_cast<ssize_t>(request.size())) {
    char buffer[512];
    for (;;) {
      const auto count = recv(fd, buffer, sizeof(buffer), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      reply.append(buffer, static_cast<std::size_t>(count));
    }
  }
  close(fd);
  return reply;
}

std::string get_health(const std::filesystem::path& path) {
  return http_exchange(path, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
}

std::string body(const std::string& reply) {
  const auto start = reply.find("\r\n\r\n");
  return start == std::string::npos ? std::string{} : reply.substr(start + 4);
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

void require_serving(const std::filesystem::path& path) {
  const auto reply = get_health(path);
  require(starts_with(reply, "HTTP/1.1 200 ") &&
              body(reply).find("\"serving\":true") != std::string::npos,
          "local health endpoint is not serving");
}

void require_rejected(const std::string& endpoint) {
  bool rejected = false;
  try {
    linuxcnc::server::detail::LocalHealthServer server(endpoint);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "occupied or unsafe health endpoint was accepted");
}

// An unrelated live listener at the health path. Connections queue in its
// backlog, so it is live without ever accepting.
class ForeignListener {
 public:
  explicit ForeignListener(const std::filesystem::path& path)
      : fd_(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    const auto address = unix_address(path);
    require(fd_ >= 0 &&
                bind(fd_, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0 &&
                listen(fd_, 16) == 0,
            "cannot start foreign test listener");
  }
  ~ForeignListener() { close(fd_); }
  ForeignListener(const ForeignListener&) = delete;
  ForeignListener& operator=(const ForeignListener&) = delete;

 private:
  int fd_;
};

bool can_connect(const std::filesystem::path& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const auto address = unix_address(path);
  const bool connected =
      fd >= 0 && connect(fd, reinterpret_cast<const sockaddr*>(&address),
                         sizeof(address)) == 0;
  if (fd >= 0) close(fd);
  return connected;
}

struct KilledChild {
  pid_t pid;

  int kill_and_wait() {
    require(kill(pid, SIGKILL) == 0, "cannot kill health owner");
    int status = 0;
    pid_t result;
    do {
      result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    require(result == pid, "cannot reap health owner");
    pid = -1;
    return status;
  }

  ~KilledChild() {
    if (pid <= 0) return;
    // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort test cleanup
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
};

// A connected client that has sent part of a request.
class PartialClient {
 public:
  PartialClient(const std::filesystem::path& path, const char* prefix)
      : fd_(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    const auto address = unix_address(path);
    require(fd_ >= 0 &&
                connect(fd_, reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)) == 0 &&
                send(fd_, prefix, std::strlen(prefix), MSG_NOSIGNAL) ==
                    static_cast<ssize_t>(std::strlen(prefix)),
            "cannot start partial client");
  }
  ~PartialClient() { close(fd_); }
  PartialClient(const PartialClient&) = delete;
  PartialClient& operator=(const PartialClient&) = delete;
  int fd() const { return fd_; }

 private:
  int fd_;
};

std::chrono::milliseconds elapsed_since(
    std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
}

// Neither a stalled nor a trickling client may hold the endpoint for longer
// than one request deadline, and shutdown must not wait for either.
void slow_clients_bounded(const std::filesystem::path& path,
                          const std::string& endpoint) {
  linuxcnc::server::detail::LocalHealthServer server(endpoint);
  {
    PartialClient stalled(path, "GET /hea");
    const auto start = std::chrono::steady_clock::now();
    require_serving(path);
    require(elapsed_since(start) < std::chrono::milliseconds(1000),
            "stalled client held the health endpoint");
  }
  {
    PartialClient trickling(path, "G");
    std::atomic<bool> done{false};
    std::thread sender([&] {
      while (!done.load()) {
        if (send(trickling.fd(), "E", 1, MSG_NOSIGNAL) != 1) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    require_serving(path);
    const auto waited = elapsed_since(start);
    done.store(true);
    sender.join();
    require(waited < std::chrono::milliseconds(1000),
            "trickling client held the health endpoint");
  }
  PartialClient stalled(path, "GET /hea");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto start = std::chrono::steady_clock::now();
  server.shutdown();
  require(elapsed_since(start) < std::chrono::milliseconds(1000),
          "shutdown waited for a stalled client");
}

void requests_routed(const std::filesystem::path& path) {
  require(starts_with(http_exchange(path, "GET /other HTTP/1.1\r\n\r\n"),
                      "HTTP/1.1 404 "),
          "unknown path was not rejected");
  require(starts_with(http_exchange(path, "POST /health HTTP/1.1\r\n\r\n"),
                      "HTTP/1.1 405 "),
          "non-GET request was not rejected");
  // The size limit must reject at once, not after the request deadline.
  const auto oversized_start = std::chrono::steady_clock::now();
  require(starts_with(http_exchange(path, std::string(2048, 'x')),
                      "HTTP/1.1 400 ") &&
              elapsed_since(oversized_start) < std::chrono::milliseconds(150),
          "oversized request was not rejected promptly");
  require(starts_with(http_exchange(path, "GET /health?probe=1 HTTP/1.1\n\n"),
                      "HTTP/1.1 200 "),
          "query string or bare-LF request was not served");
  require(starts_with(http_exchange(path, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"),
                      "HTTP/1.1 404 "),
          "gRPC/HTTP2 preface was not rejected");
}

void crash_restart(const std::filesystem::path& path,
                   const std::string& endpoint) {
  int ready[2];
  require(pipe(ready) == 0, "cannot create child readiness pipe");
  const pid_t pid = fork();
  require(pid >= 0, "cannot fork health owner");
  if (pid == 0) {
    close(ready[0]);
    try {
      linuxcnc::server::detail::LocalHealthServer server(endpoint);
      const char started = '1';
      if (write(ready[1], &started, 1) != 1) _exit(2);
      close(ready[1]);
      for (;;) pause();
    } catch (const std::exception&) {
      _exit(1);
    }
  }
  KilledChild child{pid};
  close(ready[1]);
  pollfd readable{ready[0], POLLIN, 0};
  const int result = poll(&readable, 1, 10000);
  char started = '\0';
  const auto count = result > 0 ? read(ready[0], &started, 1) : -1;
  close(ready[0]);
  require(count == 1 && started == '1', "health owner did not start");
  require_serving(path);
  require_rejected(endpoint);
  require_serving(path);
  const int status = child.kill_and_wait();
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "health owner was not killed abruptly");
  require(std::filesystem::is_socket(path), "crash did not leave a socket");
  {
    linuxcnc::server::detail::LocalHealthServer restarted(endpoint);
    require_serving(path);
    requests_routed(path);
    restarted.shutdown();
  }
  require(!std::filesystem::exists(path), "recovered socket was not cleaned");
}

void machine_reported(const std::filesystem::path& path,
                      const std::string& endpoint) {
  using linuxcnc::server::MachineHealthCache;
  auto cache = std::make_shared<MachineHealthCache>();
  linuxcnc::server::detail::LocalHealthServer server(endpoint, cache);
  require(body(get_health(path)).find("\"machine\":{\"available\":false}") !=
              std::string::npos,
          "machine reported available before any status");

  linuxcnc::server::NmlStatusSnapshot snapshot;
  snapshot.task_state = 4;
  snapshot.interp_state = 1;
  snapshot.motion_stat.traj.in_position = true;
  snapshot.ini_filename = "/machines/mill \"a\".ini";
  cache->observe(snapshot, MachineHealthCache::Clock::now() -
                               std::chrono::milliseconds(250));
  const auto reply = body(get_health(path));
  for (const char* expected :
       {"\"available\":true", "\"task_state\":4", "\"interp_state\":1",
        "\"in_position\":true", R"("ini":"/machines/mill \"a\".ini")"}) {
    require(reply.find(expected) != std::string::npos,
            "machine status field missing from health reply");
  }
  const auto age = reply.find("\"age_ms\":");
  require(age != std::string::npos && std::stol(reply.substr(age + 9)) >= 250,
          "machine status age does not reflect the observation time");

  cache->mark_unavailable();
  require(body(get_health(path)).find("\"machine\":{\"available\":false}") !=
              std::string::npos,
          "machine stayed available after status loss");
}

void live_listener_preserved(const std::filesystem::path& path,
                             const std::string& endpoint) {
  ForeignListener listener(path);
  struct stat before{}, after{};
  require(lstat(path.c_str(), &before) == 0, "cannot identify live socket");
  require_rejected(endpoint);
  require(lstat(path.c_str(), &after) == 0 && before.st_dev == after.st_dev &&
              before.st_ino == after.st_ino,
          "live listener path changed");
  require(can_connect(path), "live listener was disturbed");
  std::filesystem::remove(path);
}

void unsafe_paths_preserved(const std::filesystem::path& path,
                            const std::string& endpoint) {
  {
    std::ofstream(path) << "preserve this file";
  }
  require_rejected(endpoint);
  std::string content;
  std::getline(std::ifstream(path), content);
  require(content == "preserve this file", "occupied file was changed");
  const auto target = path.parent_path() / "other-file";
  std::filesystem::rename(path, target);
  std::filesystem::create_symlink(target, path);
  require_rejected(endpoint);
  require(std::filesystem::is_symlink(path) &&
              std::filesystem::read_symlink(path) == target,
          "occupied symlink was changed");
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  require_rejected(endpoint);
  require(std::filesystem::is_directory(path), "occupied directory changed");
  std::filesystem::remove(path);
  require(chmod(path.parent_path().c_str(), 0770) == 0,
          "cannot change health parent permissions");
  require_rejected(endpoint);
  require(!std::filesystem::exists(path), "unsafe parent acquired a socket");
  require(chmod(path.parent_path().c_str(), 0700) == 0,
          "cannot restore health parent permissions");
  const auto alias = path.parent_path() / "alias";
  std::filesystem::create_directory_symlink(path.parent_path(), alias);
  require_rejected("unix:" + (alias / path.filename()).string());
  require(std::filesystem::is_symlink(alias) && !std::filesystem::exists(path),
          "symlinked parent acquired a socket or changed");
  std::filesystem::remove(alias);
}

void replacement_preserved(const std::filesystem::path& path,
                           const std::string& endpoint) {
  auto owner =
      std::make_unique<linuxcnc::server::detail::LocalHealthServer>(endpoint);
  const auto moved = path.parent_path() / "moved.sock";
  std::filesystem::rename(path, moved);
  {
    ForeignListener replacement(path);
    owner.reset();
    require(std::filesystem::is_socket(path) && can_connect(path),
            "shutdown removed another listener's socket");
  }
  std::filesystem::remove(path);
  std::filesystem::remove(moved);
}
}  // namespace

int main() {
  using namespace linuxcnc::server;
  try {
    for (const auto* invalid :
         {"127.0.0.1:50053", "0.0.0.0:50053", "unix:relative.sock",
          "unix-abstract:health", "unix:/tmp/../health",
          "unix:/tmp/health?other", "unix:/"}) {
      DaemonConfig config;
      config.allow_plaintext = true;
      config.health_endpoint = invalid;
      require(!validate_config(config),
              "network or ambiguous health endpoint accepted");
    }
    std::string directory =
        (std::filesystem::temp_directory_path() / "linuxcnc-health-XXXXXX")
            .string();
    require(mkdtemp(directory.data()) != nullptr,
            "cannot create private health directory");
    struct Directory {
      std::filesystem::path path;
      ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{directory};
    const auto socket_path = cleanup.path / "health.sock";
    const std::string endpoint = "unix:" + socket_path.string();
    crash_restart(socket_path, endpoint);
    machine_reported(socket_path, endpoint);
    slow_clients_bounded(socket_path, endpoint);
    live_listener_preserved(socket_path, endpoint);
    unsafe_paths_preserved(socket_path, endpoint);
    replacement_preserved(socket_path, endpoint);

    for (int attempt = 0; attempt != 2; ++attempt) {
      PublicMachine machine;
      ::grpc::ServerBuilder builder;
      builder.RegisterService(&machine);
      int port = 0;
      builder.AddListeningPort("127.0.0.1:0",
                               ::grpc::InsecureServerCredentials(), &port);
      auto server = builder.BuildAndStart();
      require(server != nullptr && port > 0,
              "public test listener did not start");
      BoundedExecutor blocking(1, 8), parser(1, 8), hal(1, 8), scope(1, 8);
      AdmissionCounter streams(8), uploads(1), components(1);
      detail::ServerRuntime runtime(std::move(server), nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, streams, uploads, components,
                                    blocking, parser, hal, scope);
      require(!std::filesystem::exists(socket_path),
              "health listener started before opt-in");
      runtime.start_local_health(endpoint,
                                 std::make_shared<MachineHealthCache>());
      require_serving(socket_path);
      runtime.request_shutdown();
      require(!starts_with(get_health(socket_path), "HTTP/1.1 200 "),
              "local health remained serving during shutdown");
      runtime.finalize();
      runtime.finalize();
      require(!std::filesystem::exists(socket_path),
              "owned health socket was not removed");
    }
    std::cout << "Unix HTTP health, request routing, machine status, crash "
                 "recovery, live and unsafe path preservation, and runtime "
                 "shutdown passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

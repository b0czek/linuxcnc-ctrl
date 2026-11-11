#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "grpc/health/v1/health.grpc.pb.h"
#include "grpc/server/local_health.hpp"
#include "grpc/server/runtime.hpp"
#include "grpc/server/service.hpp"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// A distinct public endpoint result lets this test detect accidental reuse of
// the application server on the local health socket, without operating a
// machine.
class PublicMachine final : public linuxcnc::v1::MachineService::Service {
  ::grpc::Status ExecuteCommand(
      ::grpc::ServerContext*, const linuxcnc::v1::ExecuteCommandRequest*,
      linuxcnc::v1::ExecuteCommandResponse*) override {
    return ::grpc::Status::OK;
  }
};

::grpc::Status check_health(const std::shared_ptr<::grpc::Channel>& channel,
                            ::grpc::health::v1::HealthCheckResponse* response) {
  auto stub = ::grpc::health::v1::Health::NewStub(channel);
  ::grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(2));
  ::grpc::health::v1::HealthCheckRequest request;
  return stub->Check(&context, request, response);
}

::grpc::Status command(const std::shared_ptr<::grpc::Channel>& channel) {
  auto stub = linuxcnc::v1::MachineService::NewStub(channel);
  ::grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(2));
  linuxcnc::v1::ExecuteCommandRequest request;
  linuxcnc::v1::ExecuteCommandResponse response;
  return stub->ExecuteCommand(&context, request, &response);
}

void require_serving(const std::string& endpoint) {
  auto channel =
      ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
  ::grpc::health::v1::HealthCheckResponse response;
  require(
      check_health(channel, &response).ok() &&
          response.status() == ::grpc::health::v1::HealthCheckResponse::SERVING,
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

void crash_restart(const std::filesystem::path& path,
                   const std::string& endpoint) {
  // Fork before the parent starts any gRPC workers.
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
  require_serving(endpoint);
  require_rejected(endpoint);
  require_serving(endpoint);
  const int status = child.kill_and_wait();
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "health owner was not killed abruptly");
  require(std::filesystem::is_socket(path), "crash did not leave a socket");
  {
    linuxcnc::server::detail::LocalHealthServer restarted(endpoint);
    require_serving(endpoint);
    auto channel =
        ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
    require(command(channel).error_code() == ::grpc::StatusCode::UNIMPLEMENTED,
            "recovered health socket exposes machine commands");
    restarted.shutdown();
  }
  require(!std::filesystem::exists(path), "recovered socket was not cleaned");
}

std::unique_ptr<::grpc::Server> public_health_listener(
    const std::string& endpoint, PublicMachine* machine) {
  ::grpc::ServerBuilder builder;
  builder.RegisterService(machine);
  builder.AddListeningPort(endpoint, ::grpc::InsecureServerCredentials());
  auto server = builder.BuildAndStart();
  require(server != nullptr, "cannot start independent live listener");
  return server;
}

void live_listener_preserved(const std::filesystem::path& path,
                             const std::string& endpoint) {
  PublicMachine machine;
  auto listener = public_health_listener(endpoint, &machine);
  require_serving(endpoint);
  struct stat before{}, after{};
  require(lstat(path.c_str(), &before) == 0, "cannot identify live socket");
  require_rejected(endpoint);
  require(lstat(path.c_str(), &after) == 0 && before.st_dev == after.st_dev &&
              before.st_ino == after.st_ino,
          "live listener path changed");
  require_serving(endpoint);
  listener->Shutdown();
  listener->Wait();
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
  PublicMachine machine;
  auto replacement = public_health_listener(endpoint, &machine);
  require_serving(endpoint);
  owner.reset();
  require(std::filesystem::is_socket(path),
          "shutdown removed another listener's socket");
  require_serving(endpoint);
  replacement->Shutdown();
  replacement->Wait();
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
    ::grpc::EnableDefaultHealthCheckService(true);
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
      runtime.start_local_health(endpoint);
      auto local =
          ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials());
      auto remote = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                          ::grpc::InsecureChannelCredentials());
      ::grpc::health::v1::HealthCheckResponse response;
      require(check_health(local, &response).ok() &&
                  response.status() ==
                      ::grpc::health::v1::HealthCheckResponse::SERVING,
              "local standard health RPC failed");
      require(command(remote).ok(), "public test command was not reachable");
      require(command(local).error_code() == ::grpc::StatusCode::UNIMPLEMENTED,
              "local health socket exposes machine commands");
      require(check_health(remote, &response).ok() &&
                  response.status() ==
                      ::grpc::health::v1::HealthCheckResponse::SERVING,
              "local health replaced the public default health service");
      runtime.request_shutdown();
      require(!check_health(local, &response).ok() ||
                  response.status() !=
                      ::grpc::health::v1::HealthCheckResponse::SERVING,
              "local health remained serving during shutdown");
      runtime.finalize();
      runtime.finalize();
      require(!std::filesystem::exists(socket_path),
              "owned health socket was not removed");
    }
    std::cout << "Unix health RPC, command isolation, crash recovery, live and "
                 "unsafe path preservation, and runtime shutdown passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

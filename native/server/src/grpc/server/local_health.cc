#include "grpc/server/local_health.hpp"

#include <fcntl.h>
#include <grpcpp/ext/health_check_service_server_builder_option.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/server_builder_plugin.h>
#include <grpcpp/server_posix.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "grpc/health/v1/health.grpc.pb.h"
#include "linuxcnc_grpc/daemon/config.hpp"

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
    // bind never removes an occupied path. Keep path cleanup outside gRPC,
    // whose Unix listener would unlink a replacement socket on shutdown.
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

class NoGlobalPlugins final : public ::grpc::ServerBuilderOption {
  void UpdateArguments(::grpc::ChannelArguments*) override {}
  void UpdatePlugins(std::vector<std::unique_ptr<::grpc::ServerBuilderPlugin>>*
                         plugins) override {
    plugins->clear();
  }
};

class HealthService final : public ::grpc::health::v1::Health::Service {
 public:
  std::atomic<bool> serving{false};

 private:
  ::grpc::Status Check(
      ::grpc::ServerContext*,
      const ::grpc::health::v1::HealthCheckRequest* request,
      ::grpc::health::v1::HealthCheckResponse* response) override {
    if (!request->service().empty())
      return {::grpc::StatusCode::NOT_FOUND, "unknown health service"};
    response->set_status(
        serving.load(std::memory_order_acquire)
            ? ::grpc::health::v1::HealthCheckResponse::SERVING
            : ::grpc::health::v1::HealthCheckResponse::NOT_SERVING);
    return ::grpc::Status::OK;
  }
};

}  // namespace

class LocalHealthServer::Impl {
 public:
  explicit Impl(const std::string& endpoint) : endpoint_(endpoint) {
    endpoint_.listen();
    ::grpc::ServerBuilder builder;
    builder.SetOption(std::make_unique<NoGlobalPlugins>());
    builder.SetOption(
        std::make_unique<::grpc::HealthCheckServiceServerBuilderOption>(
            nullptr));
    builder.RegisterService(&service_);
    ::grpc::ResourceQuota quota;
    quota.SetMaxThreads(4);
    quota.Resize(std::size_t{4} * 1024U * 1024U);
    builder.SetResourceQuota(quota);
    builder.SetMaxReceiveMessageSize(4096);
    builder.SetMaxSendMessageSize(4096);
    server_ = builder.BuildAndStart();
    if (!server_) throw std::runtime_error("cannot bind local health endpoint");
    service_.serving.store(true, std::memory_order_release);
    try {
      accept_thread_ = std::thread([this] { accept_connections(); });
    } catch (...) {
      server_->Shutdown();
      server_->Wait();
      throw;
    }
  }

  ~Impl() {
    shutdown();
    server_->Wait();
    server_.reset();
  }

  void shutdown() {
    service_.serving.store(false, std::memory_order_release);
    if (stopped_.exchange(true)) return;
    endpoint_.stop_accepting();
    accept_thread_.join();
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
  }

 private:
  void accept_connections() {
    while (!stopped_.load()) {
      FileDescriptor client(accept4(endpoint_.fd(), nullptr, nullptr,
                                    SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (client.get() < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
            errno == ENOMEM) {
          // Descriptor or memory exhaustion is transient; keep serving.
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        service_.serving.store(false, std::memory_order_release);
        return;
      }
      if (!stopped_.load())
        ::grpc::AddInsecureChannelFromFd(server_.get(), client.release());
    }
  }

  HealthEndpoint endpoint_;
  HealthService service_;
  std::unique_ptr<::grpc::Server> server_;
  std::atomic<bool> stopped_{false};
  std::thread accept_thread_;
};

LocalHealthServer::LocalHealthServer(const std::string& endpoint)
    : impl_(std::make_unique<Impl>(endpoint)) {}
LocalHealthServer::~LocalHealthServer() = default;
void LocalHealthServer::shutdown() { impl_->shutdown(); }

}  // namespace linuxcnc::server::detail

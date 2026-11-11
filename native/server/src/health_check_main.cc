#include <grpcpp/grpcpp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "grpc/health/v1/health.grpc.pb.h"

namespace {
std::string environment(const char* name) {
  // after startup, before this single-threaded health-check client reads it.
  // NOLINTNEXTLINE(concurrency-mt-unsafe): immutable process environment
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::optional<std::string> read_file(const std::string& path) {
  if (path.empty()) return std::string{};
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

// The daemon's local Unix health endpoint speaks HTTP/1.1: GET /health
// answers 200 while serving, with a JSON body.
bool check_local_health(const std::string& path, std::string* error) {
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    *error = "invalid Unix socket path";
    return false;
  }
  address.sun_family = AF_UNIX;
  std::copy(path.begin(), path.end(), address.sun_path);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *error = "cannot create socket";
    return false;
  }
  timeval timeout{2, 0};
  // NOLINTNEXTLINE(bugprone-unused-return-value): reads stay bounded by size
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  // NOLINTNEXTLINE(bugprone-unused-return-value): best-effort send bound
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::string reply;
  const std::string request =
      "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) != 0 ||
      send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
          static_cast<ssize_t>(request.size())) {
    *error = "cannot connect";
  } else {
    char buffer[512];
    for (;;) {
      const auto count = recv(fd, buffer, sizeof(buffer), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0 || reply.size() > 16384) break;
      reply.append(buffer, static_cast<std::size_t>(count));
    }
  }
  close(fd);
  if (!error->empty()) return false;
  if (reply.rfind("HTTP/1.1 200 ", 0) != 0) {
    *error = "server is not serving";
    return false;
  }
  return true;
}

bool option_value(const std::string& argument, const char* name,
                  std::string* value) {
  const std::string prefix = std::string(name) + "=";
  if (argument.rfind(prefix, 0) != 0) return false;
  *value = argument.substr(prefix.size());
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  std::string endpoint = "127.0.0.1:50051";
  std::string ca_path = environment("LINUXCNC_GRPC_TLS_CA");
  std::string certificate_path = environment("LINUXCNC_GRPC_TLS_CERT");
  std::string private_key_path = environment("LINUXCNC_GRPC_TLS_KEY");
  std::string server_name = environment("LINUXCNC_GRPC_TLS_SERVER_NAME");
  bool have_endpoint = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      std::cout << "linuxcnc-grpc-health-check [ENDPOINT] "
                   "[--tls-ca=PATH] [--tls-certificate=PATH] "
                   "[--tls-private-key=PATH] [--tls-server-name=NAME]\n";
      return 0;
    }
    if (option_value(argument, "--tls-ca", &ca_path) ||
        option_value(argument, "--tls-certificate", &certificate_path) ||
        option_value(argument, "--tls-private-key", &private_key_path) ||
        option_value(argument, "--tls-server-name", &server_name)) {
      continue;
    }
    if (!have_endpoint && !argument.empty() && argument.front() != '-') {
      endpoint = argument;
      have_endpoint = true;
      continue;
    }
    std::cerr << "linuxcnc-grpc-health-check: unknown option " << argument
              << '\n';
    return 2;
  }

  if (endpoint.rfind("unix:", 0) == 0) {
    std::string error;
    if (!check_local_health(endpoint.substr(5), &error)) {
      std::cerr << "linuxcnc-grpc-health-check: " << error << " on " << endpoint
                << '\n';
      return 1;
    }
    return 0;
  }

  if ((certificate_path.empty() != private_key_path.empty()) ||
      (!certificate_path.empty() && ca_path.empty())) {
    std::cerr
        << "linuxcnc-grpc-health-check: client certificate and private key "
           "must be provided together with a TLS CA\n";
    return 2;
  }

  std::shared_ptr<grpc::ChannelCredentials> credentials;
  if (ca_path.empty()) {
    credentials = grpc::InsecureChannelCredentials();
  } else {
    const auto ca = read_file(ca_path);
    const auto certificate = read_file(certificate_path);
    const auto private_key = read_file(private_key_path);
    if (!ca || !certificate || !private_key) {
      std::cerr << "linuxcnc-grpc-health-check: cannot read TLS credentials\n";
      return 2;
    }
    grpc::SslCredentialsOptions options;
    options.pem_root_certs = *ca;
    options.pem_cert_chain = *certificate;
    options.pem_private_key = *private_key;
    credentials = grpc::SslCredentials(options);
  }
  grpc::ChannelArguments channel_arguments;
  if (!server_name.empty()) {
    channel_arguments.SetSslTargetNameOverride(server_name);
  }
  const auto channel =
      grpc::CreateCustomChannel(endpoint, credentials, channel_arguments);
  if (!channel->WaitForConnected(std::chrono::system_clock::now() +
                                 std::chrono::seconds(2))) {
    std::cerr << "linuxcnc-grpc-health-check: cannot connect to " << endpoint
              << '\n';
    return 1;
  }

  auto health = grpc::health::v1::Health::NewStub(channel);
  grpc::health::v1::HealthCheckRequest request;
  request.set_service("");
  grpc::health::v1::HealthCheckResponse response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(2));
  const auto status = health->Check(&context, request, &response);
  if (!status.ok() ||
      response.status() != grpc::health::v1::HealthCheckResponse::SERVING) {
    std::cerr << "linuxcnc-grpc-health-check: server is not serving on "
              << endpoint << '\n';
    return 1;
  }
  return 0;
}

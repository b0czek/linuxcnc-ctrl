#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <iostream>
#include <memory>
#include <stdexcept>

#include "grpc/health/v1/health.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"
#include "linuxcnc_grpc/listener_credentials.hpp"
#include "linuxcnc_grpc/position/telemetry.hpp"
#include "linuxcnc_grpc/program/workspace.hpp"
#include "linuxcnc_grpc/scope/telemetry.hpp"
#include "linuxcnc_grpc/telemetry_websocket_server.hpp"
#include "linuxcnc_transport/grpc_credentials.hpp"

namespace {
class TestHealth final : public ::grpc::health::v1::Health::Service {
  ::grpc::Status Check(
      ::grpc::ServerContext*, const ::grpc::health::v1::HealthCheckRequest*,
      ::grpc::health::v1::HealthCheckResponse* response) override {
    response->set_status(::grpc::health::v1::HealthCheckResponse::SERVING);
    return ::grpc::Status::OK;
  }
};
}  // namespace

int main(int argc, char** argv) {
  using namespace linuxcnc::server;
  try {
    DaemonConfig config;
    std::string error;
    bool help = false;
    if (!parse_config(argc, argv, &config, &help, &error))
      throw std::runtime_error(error);
    if (help) {
      std::cout << config_help();
      return 0;
    }
    auto credentials = load_listener_credentials(config);

    auto position = std::make_shared<PositionTelemetry>();
    auto hal = std::make_shared<HalValueTelemetry>();
    auto scope = std::make_shared<ScopeTelemetry>();
    auto workspaces = std::make_shared<ProgramWorkspaceStore>(
        config.workspace_root, config.active_program_directory);
    const auto subscription = hal->create(
        {{HalTelemetryReference{HalTelemetryItemKind::Pin, "test.value"},
          HalTelemetryType::U64}},
        std::chrono::milliseconds(50));
    if (!subscription)
      throw std::runtime_error("cannot create test subscription");
    hal->publish(subscription->subscription_id, 1,
                 {std::uint64_t{9007199254740993}});
    BoundedExecutor parser(1, 8);
    AdmissionCounter admission(16);
    TelemetryWebSocketServer telemetry(config, position, hal, scope, workspaces,
                                       parser, admission, credentials.identity);
    TestHealth health;
    ::grpc::ServerBuilder builder;
    builder.RegisterService(&health);
    linuxcnc::transport::ApplyGrpcServerHandshakeTimeout(builder);
    builder.AddListeningPort(config.endpoint, credentials.grpc);
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("cannot start test gRPC listener");
    std::cout << "READY " << subscription->websocket_path << '\n' << std::flush;
    std::string command;
    std::getline(std::cin, command);
    std::cerr << "stopping telemetry\n" << std::flush;
    telemetry.stop();
    std::cerr << "stopping grpc\n" << std::flush;
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::seconds(2));
    std::cerr << "waiting grpc\n" << std::flush;
    server->Wait();
    std::cerr << "stopped\n" << std::flush;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}

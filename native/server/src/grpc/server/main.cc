#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/resource_quota.h>
#include <pthread.h>

#include <algorithm>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "grpc/server/runtime.hpp"
#include "grpc/server/service_factories.hpp"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/grpc/server.hpp"
#include "linuxcnc_grpc/hal/value_telemetry.hpp"
#include "linuxcnc_grpc/linuxcnc/active_ini.hpp"
#include "linuxcnc_grpc/position/telemetry.hpp"
#include "linuxcnc_grpc/program/workspace.hpp"
#include "linuxcnc_grpc/scope/telemetry.hpp"
#include "linuxcnc_grpc/telemetry_websocket_server.hpp"

namespace linuxcnc::server {
namespace {

constexpr int kMaxGrpcMessageBytes = 16 * 1024 * 1024 + 64 * 1024;
constexpr int kMaxGrpcThreads = 64;

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

}  // namespace

int run_grpc_server(const DaemonConfig& config) {
  std::string error;
  if (!validate_config(config, &error)) {
    std::cerr << "linuxcnc-grpc-server: " << error << '\n';
    return 2;
  }

  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
    std::cerr << "linuxcnc-grpc-server: failed to block shutdown signals\n";
    return 2;
  }

  ::grpc::EnableDefaultHealthCheckService(true);
  if (config.reflection)
    ::grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  try {
    // Construct the LinuxCNC parser before binding either listener. IniFile is
    // cached by LinuxCNC, so this also fixes the snapshot for this session.
    auto active_ini = std::make_shared<const ActiveIni>(config.ini_file);
    auto workspaces = std::make_shared<ProgramWorkspaceStore>(
        config.workspace_root, config.active_program_directory,
        WorkspaceLimits{config.workspace_quota_bytes, config.total_quota_bytes,
                        config.workspace_ttl, config.max_workspaces,
                        config.max_workspace_entries});
    BoundedExecutor blocking(4, 128, 8);
    BoundedExecutor parser_worker(1, 8);
    BoundedExecutor hal_worker(
        1, std::max<std::size_t>(128, config.max_remote_components + 65),
        config.max_remote_components + 1);
    BoundedExecutor scope_worker(1, 128, 8);
    AdmissionCounter stream_admission(128);
    AdmissionCounter upload_admission(1);
    AdmissionCounter component_admission(config.max_remote_components);
    auto position_telemetry = std::make_shared<PositionTelemetry>(10000);
    auto hal_telemetry = std::make_shared<HalValueTelemetry>(128);
    auto scope_telemetry = std::make_shared<ScopeTelemetry>();
    auto machine = detail::make_machine_service(
        config, workspaces, position_telemetry, blocking, stream_admission);
    auto ini = detail::make_ini_service(active_ini);
    auto program =
        detail::make_program_service(config, workspaces, upload_admission);
    auto hal = detail::make_hal_service(config, hal_worker, component_admission,
                                        stream_admission, hal_telemetry);
    auto scope =
        detail::make_scope_service(config, scope_worker, scope_telemetry);
    auto telemetry_websocket = std::make_unique<TelemetryWebSocketServer>(
        config, position_telemetry, hal_telemetry, scope_telemetry, workspaces,
        parser_worker, stream_admission);

    ::grpc::ServerBuilder builder;
    ::grpc::ResourceQuota resource_quota;
    resource_quota.Resize(std::size_t{256} * 1024U * 1024U);
    resource_quota.SetMaxThreads(kMaxGrpcThreads);
    builder.SetResourceQuota(resource_quota);
    builder.SetMaxReceiveMessageSize(kMaxGrpcMessageBytes);
    builder.SetMaxSendMessageSize(kMaxGrpcMessageBytes);

    std::shared_ptr<::grpc::ServerCredentials> credentials;
    if (!config.tls) {
      credentials = ::grpc::InsecureServerCredentials();
    } else {
      ::grpc::SslServerCredentialsOptions options;
      options.pem_key_cert_pairs.push_back({read_file(config.tls_private_key),
                                            read_file(config.tls_certificate)});
      options.pem_root_certs = read_file(config.tls_client_ca);
      if (options.pem_key_cert_pairs.front().private_key.empty() ||
          options.pem_key_cert_pairs.front().cert_chain.empty()) {
        std::cerr
            << "linuxcnc-grpc-server: failed to read TLS key/certificate\n";
        return 2;
      }
      if (config.mtls) {
        options.client_certificate_request =
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
      }
      credentials = ::grpc::SslServerCredentials(options);
    }

    builder.AddListeningPort(config.endpoint, credentials);
    builder.RegisterService(machine->service());
    builder.RegisterService(ini->service());
    builder.RegisterService(program->service());
    builder.RegisterService(hal->service());
    builder.RegisterService(scope->service());
    auto server = builder.BuildAndStart();
    if (!server) {
      std::cerr << "linuxcnc-grpc-server: failed to bind " << config.endpoint
                << '\n';
      return 2;
    }
    std::cout << "linuxcnc-grpc-server listening on " << config.endpoint
              << " and telemetry on ws://" << config.telemetry_endpoint << '\n'
              << std::flush;

    detail::ServerRuntime runtime(
        std::move(server), std::move(machine), std::move(ini),
        std::move(program), std::move(hal), std::move(scope),
        std::move(telemetry_websocket), std::move(position_telemetry),
        std::move(hal_telemetry), stream_admission, upload_admission,
        component_admission, blocking, parser_worker, hal_worker, scope_worker);
    runtime.start_control_thread(shutdown_signals);
    runtime.wait();
    runtime.finalize();
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "linuxcnc-grpc-server: initialization failed: "
              << exception.what() << '\n';
    return 2;
  }
}

}  // namespace linuxcnc::server

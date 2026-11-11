#pragma once

#include <memory>

#include "grpc/server/service.hpp"

namespace linuxcnc::server {

class BoundedExecutor;
class AdmissionCounter;
class PositionTelemetry;
class HalValueTelemetry;
class ScopeTelemetry;
class ActiveIni;
class MachineHealthCache;
class ProgramWorkspaceStore;
struct DaemonConfig;

}  // namespace linuxcnc::server

namespace linuxcnc::server::detail {

std::unique_ptr<ManagedGrpcService> make_ini_service(
    std::shared_ptr<const ActiveIni> ini);

std::unique_ptr<ManagedGrpcService> make_machine_service(
    const DaemonConfig& config,
    std::shared_ptr<ProgramWorkspaceStore> workspaces,
    std::shared_ptr<PositionTelemetry> positions, BoundedExecutor& blocking,
    AdmissionCounter& stream_admission,
    std::shared_ptr<MachineHealthCache> health);

std::unique_ptr<ManagedGrpcService> make_program_service(
    const DaemonConfig& config, std::shared_ptr<ProgramWorkspaceStore> store,
    AdmissionCounter& upload_admission);

std::unique_ptr<ManagedGrpcService> make_hal_service(
    const DaemonConfig& config, BoundedExecutor& worker,
    AdmissionCounter& component_admission, AdmissionCounter& stream_admission,
    std::shared_ptr<HalValueTelemetry> telemetry);

std::unique_ptr<ManagedGrpcService> make_scope_service(
    const DaemonConfig& config, BoundedExecutor& worker,
    std::shared_ptr<ScopeTelemetry> scope_telemetry);

}  // namespace linuxcnc::server::detail

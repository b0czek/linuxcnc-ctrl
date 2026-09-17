#pragma once

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "linuxcnc/v1/hal.grpc.pb.h"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc/v1/program.grpc.pb.h"

namespace grpc_live {

using linuxcnc::v1::ExecuteCommandRequest;
using linuxcnc::v1::ExecuteCommandResponse;
using linuxcnc::v1::GetStatusResponse;

std::shared_ptr<grpc::Channel> make_channel(const std::string& endpoint);
std::pair<std::string, std::string> split_endpoint(const std::string& endpoint);

linuxcnc::v1::GetStatusResponse get_status_with_retry(
    linuxcnc::v1::MachineService::Stub* machine);
linuxcnc::v1::GetStatusResponse wait_for_optional_stop(
    linuxcnc::v1::MachineService::Stub* machine, bool expected);
linuxcnc::v1::ExecuteCommandResponse set_optional_stop(
    linuxcnc::v1::MachineService::Stub* machine, bool enabled,
    linuxcnc::v1::WaitPolicy wait_policy);
linuxcnc::v1::ExecuteCommandResponse execute_completed(
    linuxcnc::v1::MachineService::Stub* machine,
    linuxcnc::v1::ExecuteCommandRequest request);
linuxcnc::v1::ExecuteCommandResponse execute_accepted(
    linuxcnc::v1::MachineService::Stub* machine,
    linuxcnc::v1::ExecuteCommandRequest request);

void verify_position_telemetry(const std::string& endpoint);
void verify_hal_value_subscription_lifecycle(
    linuxcnc::v1::HalService::Stub* hal, const std::string& telemetry_endpoint,
    const std::string& pin_name);
void verify_moving_status_cadence(linuxcnc::v1::MachineService::Stub* machine);
void expect_command_error(linuxcnc::v1::MachineService::Stub* machine,
                          const linuxcnc::v1::ExecuteCommandRequest& request,
                          grpc::StatusCode expected);
void verify_priority_admission(linuxcnc::v1::MachineService::Stub* machine);

std::string read_file(const std::string& path);
linuxcnc::v1::UploadWorkspaceResponse upload_program(
    linuxcnc::v1::ProgramService::Stub* program, const std::string& path,
    const std::string& contents);
void verify_preview_stream(const std::string& telemetry_endpoint,
                           const std::string& workspace_id,
                           const std::string& relative_path,
                           std::size_t batch_limit);
void verify_chunked_preview_stream(linuxcnc::v1::ProgramService::Stub* program,
                                   const std::string& telemetry_endpoint,
                                   std::size_t batch_limit);

int probe_reacquire(const std::string& endpoint);
int hold_shutdown(const std::string& endpoint);
bool verify_hal_and_scope(const std::shared_ptr<grpc::Channel>& channel,
                          const std::string& telemetry_endpoint);

}  // namespace grpc_live

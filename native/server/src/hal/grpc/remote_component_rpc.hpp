#pragma once

#include <grpcpp/support/server_callback.h>

#include <chrono>
#include <memory>

#include "linuxcnc/v1/hal.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/hal/adapter.hpp"

namespace linuxcnc::server::detail {

// Owns RunComponent transport reactors and delegates serialized proxy state to
// RemoteComponentRegistry. The parent HAL service only starts streams and
// drives the timer/shutdown hooks.
class RemoteComponentRpc final {
 public:
  RemoteComponentRpc(LinuxCncHalAdapter& adapter, BoundedExecutor& worker,
                     AdmissionCounter& component_admission,
                     AdmissionCounter& stream_admission,
                     ActiveCallbackRegistry& callbacks,
                     const DaemonConfig& config);
  ~RemoteComponentRpc();

  RemoteComponentRpc(const RemoteComponentRpc&) = delete;
  RemoteComponentRpc& operator=(const RemoteComponentRpc&) = delete;

  ::grpc::ServerBidiReactor<linuxcnc::v1::HalComponentClientMessage,
                            linuxcnc::v1::HalComponentServerMessage>*
  run();
  void sample(std::chrono::steady_clock::time_point now);
  void tick_streams(std::chrono::steady_clock::time_point now);
  void shutdown_registry();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server::detail

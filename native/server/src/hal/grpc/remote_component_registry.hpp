#pragma once

#include <grpcpp/support/status.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "linuxcnc/v1/hal.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/hal/adapter.hpp"

namespace linuxcnc::server::detail {

struct RemoteComponentCallbacks {
  std::function<void(linuxcnc::v1::HalComponentServerMessage)> offer_delta;
  std::function<void(std::uint64_t)> heartbeat_timeout;
};

struct RemoteComponentResult {
  ::grpc::Status status;
  std::optional<linuxcnc::v1::HalComponentServerMessage> response;
  std::string proxy_id;
  std::uint64_t generation = 0;
  bool close = false;
};

// Owns every retained remote HAL proxy. All methods must be called on the
// serialized HAL worker; transport reactors retain only proxy ID/generation
// handles and callbacks guarded by their own lifetime gates.
class RemoteComponentRegistry final {
 public:
  RemoteComponentRegistry(LinuxCncHalAdapter& adapter,
                          AdmissionCounter& component_admission,
                          const DaemonConfig& config);
  ~RemoteComponentRegistry();

  RemoteComponentRegistry(const RemoteComponentRegistry&) = delete;
  RemoteComponentRegistry& operator=(const RemoteComponentRegistry&) = delete;

  RemoteComponentResult consume(
      const std::string& attached_id, std::uint64_t attached_generation,
      RemoteComponentCallbacks callbacks,
      const linuxcnc::v1::HalComponentClientMessage& request);
  void detach(const std::string& proxy_id, std::uint64_t generation);
  void sample(std::chrono::steady_clock::time_point now);
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server::detail

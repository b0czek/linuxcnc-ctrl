#pragma once

#include <cstdint>
#include <optional>

#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"

namespace linuxcnc::server::detail {

struct EncodedStatus {
  ::linuxcnc::v1::LinuxCNCStat message;
};

EncodedStatus encode_status(const NmlStatusSnapshot& source);

std::optional<::linuxcnc::v1::LinuxCNCStatDelta> make_status_delta(
    const EncodedStatus& previous, const EncodedStatus& current,
    std::uint64_t sequence);

}  // namespace linuxcnc::server::detail

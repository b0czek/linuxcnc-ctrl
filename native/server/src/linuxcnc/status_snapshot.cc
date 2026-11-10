#include "linuxcnc_grpc/linuxcnc/status_snapshot.hpp"

#include <stdexcept>

#ifdef LINUXCNC_HAS_PASSIVE_STATUS
#include <algorithm>

#include "emc.hh"
#include "emc_nml.hh"
#include "libnml/buffer/shmem_snapshot.hh"
#endif

namespace linuxcnc::server {

class PassiveStatusReader::Impl {
 public:
#ifdef LINUXCNC_HAS_PASSIVE_STATUS
  explicit Impl(const std::filesystem::path& configuration)
      : channel(CMS_SHMEM_SNAPSHOT::open("emcStatus", "xemc",
                                         configuration.c_str())) {
    if (!channel)
      throw std::runtime_error("passive status channel unavailable");
  }
  std::unique_ptr<CMS_SHMEM_SNAPSHOT> channel;
#else
  explicit Impl(const std::filesystem::path&) {
    throw std::runtime_error("passive status API unavailable in this build");
  }
#endif
};

PassiveStatusReader::PassiveStatusReader(
    const std::filesystem::path& configuration)
    : impl_(std::make_unique<Impl>(configuration)) {}
PassiveStatusReader::~PassiveStatusReader() = default;

PassiveStatusSnapshot PassiveStatusReader::read() {
#ifdef LINUXCNC_HAS_PASSIVE_STATUS
  EMC_STAT status;
  long size = 0;
  if (impl_->channel->peek(&status, static_cast<long>(sizeof(status)), size,
                           0.05) != CMS_READ_OK ||
      size != static_cast<long>(sizeof(status))) {
    throw std::runtime_error("passive status snapshot failed");
  }
  if (status._type != EMC_STAT_TYPE ||
      status.size != static_cast<long>(sizeof(EMC_STAT))) {
    throw std::runtime_error(
        "passive status message has incompatible type/size");
  }
  const auto end = std::find(std::begin(status.task.ini_filename),
                             std::end(status.task.ini_filename), '\0');
  if (end == std::end(status.task.ini_filename)) {
    throw std::runtime_error("passive status INI identity is unterminated");
  }
  const auto state = status.task.state;
  if (state != EMC_TASK_STATE::ESTOP && state != EMC_TASK_STATE::ESTOP_RESET &&
      state != EMC_TASK_STATE::OFF && state != EMC_TASK_STATE::ON) {
    throw std::runtime_error("passive status task state is unknown");
  }
  return {status.task.taskbeat, static_cast<std::int32_t>(state),
          static_cast<std::int32_t>(status.task.interpState),
          status.motion.traj.inpos, std::string(status.task.ini_filename, end)};
#else
  throw std::runtime_error("passive status API unavailable in this build");
#endif
}

}  // namespace linuxcnc::server

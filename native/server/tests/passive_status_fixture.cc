#include <poll.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "emc.hh"
#include "emc_nml.hh"
#include "libnml/buffer/shmem_snapshot.hh"
#include "libnml/cms/cms_cfg.hh"
#include "libnml/nml/stat_msg.hh"
#include "linuxcnc_grpc/linuxcnc/status_snapshot.hpp"

namespace {
constexpr key_t kFixtureKey = 1930000001;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int writer(const char* configuration, const char* ini) {
  auto channel = std::make_unique<RCS_STAT_CHANNEL>(
      emcFormat, "emcStatus", "fixture_writer", configuration);
  require(channel->valid(), "fixture writer configuration failed");
  const int shared = ::shmget(kFixtureKey, 20480, 0);
  const int semaphore = ::semget(kFixtureKey, 1, 0);
  require(shared >= 0 && semaphore >= 0, "fixture IPC missing");
  void* mapping = ::shmat(shared, nullptr, 0);
  // NOLINTNEXTLINE(performance-no-int-to-ptr): SysV shmat failure sentinel.
  require(mapping != reinterpret_cast<void*>(-1), "fixture mapping failed");
  EMC_STAT message;
  message.task.state = EMC_TASK_STATE::ON;
  message.task.interpState = EMC_TASK_INTERP::IDLE;
  message.motion.traj.inpos = true;
  std::snprintf(message.task.ini_filename, sizeof(message.task.ini_filename),
                "%s", ini);
  bool paused = false;
  bool held = false;
  bool stopped = false;
  ++message.task.taskbeat;
  require(channel->write(&message) == 0, "fixture initial publish failed");
  std::cout << "ready\n" << std::flush;
  while (!stopped) {
    if (!paused && !held) {
      ++message.task.taskbeat;
      require(channel->write(&message) == 0, "fixture publish failed");
    }
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (::poll(&input, 1, 10) <= 0) continue;
    std::string command;
    if (!std::getline(std::cin, command)) break;
    if (command == "pause")
      paused = true;
    else if (command == "resume")
      paused = false;
    else if (command == "lock") {
      paused = true;
      sembuf operation{0, -1, IPC_NOWAIT};
      require(!held && ::semop(semaphore, &operation, 1) == 0,
              "fixture hold failed");
      held = true;
    } else if (command == "unlock") {
      sembuf operation{0, 1, IPC_NOWAIT};
      require(held && ::semop(semaphore, &operation, 1) == 0,
              "fixture release failed");
      held = false;
    } else if (command == "foreign") {
      paused = true;
      const char foreign[] = "UNRELATED_APPLICATION_STATEXXXXX";
      std::memcpy(mapping, foreign, 32);
    } else if (command == "value") {
      std::cout << ::semctl(semaphore, 0, GETVAL) << '\n' << std::flush;
      continue;
    } else if (command == "attachments") {
      struct shmid_ds details{};
      require(::shmctl(shared, IPC_STAT, &details) == 0, "fixture stat failed");
      std::cout << details.shm_nattch << '\n' << std::flush;
      continue;
    } else if (command == "header") {
      for (std::size_t i = 0; i < 32; ++i) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x",
                      static_cast<unsigned char*>(mapping)[i]);
        std::cout << hex;
      }
      std::cout << '\n' << std::flush;
      continue;
    } else if (command == "reset") {
      require(!held && ::semctl(semaphore, 0, SETVAL, 1) == 0,
              "fixture reset failed");
    } else if (command == "remove") {
      paused = true;
      require(::semctl(semaphore, 0, IPC_RMID) == 0, "fixture removal failed");
    } else if (command == "stop")
      stopped = true;
    else
      throw std::runtime_error("unknown fixture command");
    std::cout << "ok\n" << std::flush;
  }
  if (held) {
    sembuf operation{0, 1, IPC_NOWAIT};
    ::semop(semaphore, &operation, 1);
  }
  ::shmdt(mapping);
  return 0;
}

int probe(const std::string& mode, const char* configuration) {
  if (mode == "read") {
    linuxcnc::server::PassiveStatusReader reader(configuration);
    const auto status = reader.read();
    std::cout << status.task_heartbeat << ' ' << status.task_state << ' '
              << status.ini_filename << '\n';
    return 0;
  }
  auto channel = CMS_SHMEM_SNAPSHOT::open("emcStatus", "xemc", configuration);
  require(static_cast<bool>(channel), "passive fixture open failed");
  if (mode == "api-read" || mode == "alarm-read") {
    if (mode == "alarm-read") {
      itimerval timer{};
      timer.it_value.tv_usec = 50000;
      require(::setitimer(ITIMER_REAL, &timer, nullptr) == 0,
              "fixture timer failed");
    }
    EMC_STAT message;
    long size = 0;
    require(channel->peek(&message, sizeof(message), size, 1.0) == CMS_READ_OK,
            "fixture snapshot failed");
    std::cout << "read\n";
    return 0;
  }
  if (mode == "forbidden") {
    // Deliberately bypass compile-time private inheritance to exercise the
    // access owner's runtime rejection of inherited mutable/read paths.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* base = reinterpret_cast<CMS*>(channel.get());
    EMC_STAT message;
    require(base->read() == CMS_PERMISSIONS_ERROR &&
                base->clear() == CMS_PERMISSIONS_ERROR &&
                base->write(&message) == CMS_PERMISSIONS_ERROR,
            "non-PEEK access was permitted");
    std::cout << "refused\n";
    return 0;
  }
  if (mode == "mapping-write") {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
      if (line.find("/SYSV") == std::string::npos ||
          line.find("r--s") == std::string::npos)
        continue;
      std::istringstream fields(line);
      std::uintptr_t start = 0;
      fields >> std::hex >> start;
      require(start != 0, "fixture mapping address missing");
      // Kernel enforcement is the observable contract: this write must fault.
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      *reinterpret_cast<volatile char*>(start) = 'X';
      throw std::runtime_error("read-only mapping was writable");
    }
    throw std::runtime_error("read-only mapping not found");
  }
  throw std::runtime_error("unknown fixture probe");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if ((argc == 4 || argc == 6) && std::string(argv[1]) == "writer")
      return writer(argv[2], argv[3]);
    if (argc == 3 && std::string(argv[1]) == "initialize") {
      RCS_STAT_CHANNEL channel(emcFormat, "emcStatus", "fixture_writer",
                               argv[2]);
      require(channel.valid(), "fixture initialization failed");
      std::cout << "initialized\n" << std::flush;
      std::string command;
      std::getline(std::cin, command);
      return 0;
    }
    if (argc == 3) return probe(argv[1], argv[2]);
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

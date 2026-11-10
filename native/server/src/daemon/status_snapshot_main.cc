#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "linuxcnc_grpc/linuxcnc/status_snapshot.hpp"

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: linuxcnc-status-snapshot CONFIG");
    const auto parent = ::getppid();
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || parent == 1 ||
        ::getppid() != parent) {
      throw std::runtime_error("cannot establish snapshot parent lifecycle");
    }
    sigset_t alarm_signal;
    ::sigemptyset(&alarm_signal);
    ::sigaddset(&alarm_signal, SIGALRM);
    if (::pthread_sigmask(SIG_UNBLOCK, &alarm_signal, nullptr) != 0) {
      throw std::runtime_error("cannot unblock snapshot alarm");
    }
    std::signal(SIGALRM, SIG_DFL);
    ::alarm(1);
    linuxcnc::server::PassiveStatusReader reader(argv[1]);
    auto status = reader.read();
    const auto first = status.task_heartbeat;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    do {
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("passive task status is stale");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      status = reader.read();
    } while (status.task_heartbeat == first);
    std::cout << "linuxcnc-status-v1 " << status.task_heartbeat << ' '
              << status.task_state << ' '
              << static_cast<int>(status.in_position) << ' '
              << status.interpreter_state << ' ';
    for (const unsigned char value : status.ini_filename) {
      std::cout << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(value);
    }
    std::cout << '\n' << std::flush;
    if (!std::cout) throw std::runtime_error("cannot write passive status");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

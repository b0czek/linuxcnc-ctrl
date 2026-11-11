#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "bytes.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace linuxcnc::transport {
namespace {
using Clock = std::chrono::steady_clock;
class Socket {
 public:
  Socket() : fd_(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    detail::Require(fd_ >= 0, "cannot create signer socket");
  }
  ~Socket() { close(fd_); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int fd() const { return fd_; }

 private:
  int fd_;
};
void Ready(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - Clock::now())
                          .count();
    detail::Require(left > 0, "external signer deadline exceeded");
    pollfd pending{fd, events, 0};
    const int result = poll(&pending, 1, static_cast<int>(left));
    if (result < 0 && errno == EINTR) continue;
    detail::Require(result > 0, "external signer timed out or poll failed");
    detail::Require((pending.revents & POLLNVAL) == 0, "invalid signer socket");
    return;
  }
}
// A blocking AF_UNIX connect sleeps until the listener's backlog has room,
// where a nonblocking one fails at once with EAGAIN. SO_SNDTIMEO bounds that
// wait by the request deadline; a zero timeout would mean no bound, so the
// remaining time is checked first. I/O is nonblocking afterwards.
void Connect(int fd, const sockaddr_un& address, Clock::time_point deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                          deadline - Clock::now())
                          .count();
    detail::Require(left > 0, "external signer deadline exceeded");
    const timeval timeout{static_cast<time_t>(left / 1000000),
                          static_cast<suseconds_t>(left % 1000000)};
    detail::Require(
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
        "cannot bound external signer connect");
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0)
      break;
    if (errno == EINTR) continue;
    detail::Require(errno != EAGAIN, "external signer deadline exceeded");
    throw std::runtime_error("cannot connect to external signer");
  }
  const int flags = fcntl(fd, F_GETFL);
  detail::Require(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
                  "cannot configure external signer socket");
}
void Transfer(int fd, unsigned char* data, size_t size, bool send_data,
              Clock::time_point deadline) {
  while (size != 0) {
    Ready(fd, send_data ? POLLOUT : POLLIN, deadline);
    const auto n = send_data ? send(fd, data, size, MSG_NOSIGNAL)
                             : recv(fd, data, size, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    detail::Require(n > 0, "external signer I/O failed or closed");
    data += n;
    size -= static_cast<size_t>(n);
  }
}
}  // namespace

std::vector<unsigned char> RequestServerSignature(
    const ExternalKeyReference& key, const TlsServerSignRequest& request) {
  // References are validated when decoded or loaded; only guard the copy into
  // sun_path here rather than revalidating the public point per handshake.
  detail::Require(!key.socket_path.empty() &&
                      key.socket_path.size() < sizeof(sockaddr_un::sun_path),
                  "invalid signer socket path");
  detail::Require(key.key_id == request.key_id, "signer key id mismatch");
  auto wire = EncodeSignRequest(request);
  const auto deadline = Clock::now() + kSignerTimeout;
  Socket connection;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, key.socket_path.c_str(),
              key.socket_path.size() + 1);
  Connect(connection.fd(), address, deadline);
  Transfer(connection.fd(), wire.data(), wire.size(), true, deadline);
  detail::Bytes reply(2);
  Transfer(connection.fd(), reply.data(), reply.size(), false, deadline);
  detail::Reader header(detail::View(reply));
  const auto size = header.U16();
  detail::Require(size >= 5 && size + 2 <= kMaxSignerRecord,
                  "invalid signer response length");
  reply.resize(size + 2);
  Transfer(connection.fd(), reply.data() + 2, size, false, deadline);
  return DecodeSignResponse(detail::View(reply));
}
}  // namespace linuxcnc::transport

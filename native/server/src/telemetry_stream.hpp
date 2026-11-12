#pragma once

#include <boost/asio/post.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <utility>
#include <variant>

namespace linuxcnc::server::detail {

// One AsyncStream keeps the route/session implementation common to WS and WSS.
// A TLS stream never dispatches I/O through its unencrypted socket.
class TelemetryStream {
  using Socket = boost::asio::ip::tcp::socket;
  using Tls = boost::asio::ssl::stream<Socket>;

 public:
  using executor_type = Socket::executor_type;
  TelemetryStream(Socket socket, boost::asio::ssl::context* context)
      : stream_(std::in_place_type<Socket>, std::move(socket)) {
    if (context) {
      auto plain = std::move(std::get<Socket>(stream_));
      stream_.emplace<Tls>(std::move(plain), *context);
    }
  }
  executor_type get_executor() {
    return std::visit([](auto& stream) { return stream.get_executor(); },
                      stream_);
  }
  Socket& next_layer() {
    if (auto* tls = std::get_if<Tls>(&stream_)) return tls->next_layer();
    return std::get<Socket>(stream_);
  }

  template <typename Handler>
  void async_handshake(Handler&& handler) {
    if (auto* tls = std::get_if<Tls>(&stream_)) {
      tls->async_handshake(boost::asio::ssl::stream_base::server,
                           std::forward<Handler>(handler));
    } else {
      boost::asio::post(get_executor(),
                        [handler = std::forward<Handler>(handler)]() mutable {
                          handler(boost::beast::error_code{});
                        });
    }
  }
  template <typename Buffers, typename Handler>
  void async_read_some(const Buffers& buffers, Handler&& handler) {
    std::visit(
        [&](auto& stream) {
          stream.async_read_some(buffers, std::forward<Handler>(handler));
        },
        stream_);
  }
  template <typename Buffers, typename Handler>
  void async_write_some(const Buffers& buffers, Handler&& handler) {
    std::visit(
        [&](auto& stream) {
          stream.async_write_some(buffers, std::forward<Handler>(handler));
        },
        stream_);
  }
  template <typename Handler>
  void async_teardown(boost::beast::role_type role, Handler&& handler) {
    std::visit(
        [&](auto& stream) {
          using boost::beast::async_teardown;
          using boost::beast::websocket::async_teardown;
          async_teardown(role, stream, std::forward<Handler>(handler));
        },
        stream_);
  }

 private:
  std::variant<Socket, Tls> stream_;
};

template <typename Handler>
void async_teardown(boost::beast::role_type role, TelemetryStream& stream,
                    Handler&& handler) {
  stream.async_teardown(role, std::forward<Handler>(handler));
}
}  // namespace linuxcnc::server::detail

#include "publisher.h"
#include "logger.h"
#include <fmt/format.h>

namespace spectator {

SpectatordPublisher::SpectatordPublisher(std::string_view endpoint,
                                         std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)),
      udp_socket_(io_context_),
      local_socket_(io_context_) {
  if (absl::StartsWith(endpoint, "unix:")) {
    setup_unix_domain(endpoint.substr(5));
  } else if (absl::StartsWith(endpoint, "udp:")) {
    auto pos = 4;
    // if the user used udp://foo:1234 instead of udp:foo:1234
    // adjust accordingly
    if (endpoint.substr(pos, 2) == "//") {
      pos += 2;
    }
    setup_udp(endpoint.substr(pos));
  } else if (endpoint != "disabled") {
    logger_->warn(
        "Unknown endpoint: {}. Expecting: 'unix:/path/to/socket'"
        " or 'udp:hostname:port' - Will not send metrics",
        endpoint);
  }
}

void SpectatordPublisher::local_reconnect(std::string_view path) {
  using endpoint_t = asio::local::datagram_protocol::endpoint;
  try {
    if (local_socket_.is_open()) {
      local_socket_.close();
    }
    local_socket_.open();
    local_socket_.connect(endpoint_t(std::string(path)));
  } catch (std::exception& e) {
    logger_->warn("Unable to connect to {}: {}", path, e.what());
  }
}

void SpectatordPublisher::setup_unix_domain(std::string_view path) {
  local_reconnect(path);
  // get a copy of the file path
  std::string local_path{path};
  sender_ = [local_path, this](std::string_view msg) {
    for (auto i = 0; i < 3; ++i) {
      try {
        local_socket_.send(asio::buffer(msg));
        logger_->trace("Sent (local): {}", msg);
        break;
      } catch (std::exception& e) {
        local_reconnect(local_path);
        logger_->warn("Unable to send {} - attempt {}/3 ({})", msg, i,
                      e.what());
      }
    }
  };
}

inline asio::ip::udp::endpoint resolve_host_port(
    asio::io_context& io_context,  // NOLINT
    std::string_view host_port) {
  using asio::ip::udp;
  udp::resolver resolver{io_context};

  auto end_host = host_port.find(':');
  if (end_host == std::string_view::npos) {
    auto err = fmt::format(
        "Unable to parse udp endpoint: '{}'. Expecting hostname:port",
        host_port);
    throw std::runtime_error(err);
  }

  auto host = host_port.substr(0, end_host);
  auto port = host_port.substr(end_host + 1);
  return *resolver.resolve(udp::v6(), host, port);
}

void SpectatordPublisher::udp_reconnect(
    const asio::ip::udp::endpoint& endpoint) {
  try {
    if (udp_socket_.is_open()) {
      udp_socket_.close();
    }
    udp_socket_.open(asio::ip::udp::v6());
    udp_socket_.connect(endpoint);
  } catch (std::exception& e) {
    logger_->warn("Unable to connect to {}: {}", endpoint.address().to_string(),
                  endpoint.port());
  }
}

void SpectatordPublisher::setup_udp(std::string_view host_port) {
  auto endpoint = resolve_host_port(io_context_, host_port);
  udp_reconnect(endpoint);
  sender_ = [endpoint, this](std::string_view msg) {
    for (auto i = 0; i < 3; ++i) {
      try {
        udp_socket_.send(asio::buffer(msg));
        logger_->trace("Sent (udp): {}", msg);
        break;
      } catch (std::exception& e) {
        logger_->warn("Unable to send {} - attempt {}/3", msg, i);
        udp_reconnect(endpoint);
      }
    }
  };
}
}  // namespace spectator
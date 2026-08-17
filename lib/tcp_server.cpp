/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "tcp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace gr::difi {

/**
 * @brief Creates a TCP server that listens on the specified IP address and
 * port.
 *
 * @param ip_addr the IP address to bind the server to
 * @param port the port number to listen on
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
tcp_server::tcp_server(std::string ip_addr, uint16_t port,
                       size_t recv_buffer_size, size_t send_buffer_size)
    : tcp_transport(recv_buffer_size, send_buffer_size), m_listener(-1) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_addr.c_str(), &addr) != 1) {
    throw std::invalid_argument("Not a valid IPv4 address: " + ip_addr);
  }
  listen_socket(addr.s_addr, port);
}

/**
 * @brief Creates a TCP server that listens on the specified port and binds to
 * all available network interfaces.
 *
 * @param port the port number to listen on
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
tcp_server::tcp_server(uint16_t port, size_t recv_buffer_size,
                       size_t send_buffer_size)
    : tcp_transport(recv_buffer_size, send_buffer_size), m_listener(-1) {
  listen_socket(INADDR_ANY, port);
}

tcp_server::~tcp_server() {
  if (m_listener >= 0) {
    ::close(m_listener);
  }
}

/**
 * @brief Opens the listening socket and binds it to a local endpoint.
 *
 * @param addr the local address to bind to, in network byte order
 * @param port the port number to listen on
 */
void tcp_server::listen_socket(in_addr_t addr, uint16_t port) {
  m_listener = create_socket(SOCK_STREAM, IPPROTO_TCP);

  /* The object is not fully constructed until this returns, so its destructor
   * will not run and the socket has to be released by hand on failure. */
  try {
    /* Without this, a restart fails to bind while the previous socket is still
     * in the TIME_WAIT state. */
    const int enable = 1;
    if (::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &enable,
                     sizeof(enable)) < 0) {
      throw std::runtime_error(
          errno_msg("Could not allow the port to be reused"));
    }

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = addr;

    if (::bind(m_listener, reinterpret_cast<const struct sockaddr *>(&local),
               sizeof(local)) < 0) {
      throw std::runtime_error(errno_msg("Could not bind to port " +
                                         std::to_string(port) +
                                         ", it may already be in use"));
    }

    /* One peer is served at a time, so the backlog only has to hold the next
     * peer waiting to replace it. */
    if (::listen(m_listener, 1) < 0) {
      throw std::runtime_error(
          errno_msg("Could not listen on port " + std::to_string(port)));
    }
  } catch (...) {
    ::close(m_listener);
    m_listener = -1;
    throw;
  }
}

/**
 * @brief Accepts a peer if one is waiting to connect.
 *
 * Waits for at most the receive timeout, so that a caller polling this
 * transport is not blocked while no peer arrives.
 *
 * @return true if a peer is now connected
 */
bool tcp_server::ensure_connected() {
  struct pollfd pfd = {};
  pfd.fd = m_listener;
  pfd.events = POLLIN;

  int rc = ::poll(&pfd, 1, RECV_TIMEOUT_US / 1000);
  if (rc <= 0 || (pfd.revents & POLLIN) == 0) {
    return false;
  }

  int client = ::accept(m_listener, nullptr, nullptr);
  if (client < 0) {
    return false;
  }

  /* A socket returned by accept() does not inherit the options of the
   * listening socket, so it has to be configured on its own. */
  try {
    apply_socket_options(client);
  } catch (const std::exception &) {
    ::close(client);
    return false;
  }

  m_socket = client;
  return true;
}

} // namespace gr::difi

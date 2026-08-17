/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "tcp_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace gr::difi {

/**
 * @brief Creates a TCP client for the given remote endpoint.
 *
 * No connection is attempted here. Only the address is checked, so that a
 * mistyped address is reported immediately while a peer that is not listening
 * yet is not treated as an error.
 *
 * @param ip_addr the IP address of the remote endpoint
 * @param port the port number of the remote endpoint
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
tcp_client::tcp_client(std::string ip_addr, uint16_t port,
                       size_t recv_buffer_size, size_t send_buffer_size)
    : tcp_transport(recv_buffer_size, send_buffer_size), m_remote({}) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_addr.c_str(), &addr) != 1) {
    throw std::invalid_argument("Not a valid IPv4 address: " + ip_addr);
  }

  m_remote.sin_family = AF_INET;
  m_remote.sin_port = htons(port);
  m_remote.sin_addr = addr;
}

/**
 * @brief Connects to the remote endpoint.
 *
 * The socket is placed in non blocking mode for the attempt, so that a peer
 * which does not answer cannot block the caller for longer than the receive
 * timeout. It is placed back in blocking mode afterwards, because the rest of
 * the transport relies on blocking calls bounded by that same timeout.
 *
 * @return true if the connection was established
 */
bool tcp_client::ensure_connected() {
  int fd;
  try {
    fd = create_socket(SOCK_STREAM, IPPROTO_TCP);
  } catch (const std::exception &) {
    return false;
  }

  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(fd);
    return false;
  }

  bool connected = false;
  if (::connect(fd, reinterpret_cast<const struct sockaddr *>(&m_remote),
                sizeof(m_remote)) == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    if (::poll(&pfd, 1, RECV_TIMEOUT_US / 1000) > 0 && (pfd.revents & POLLOUT)) {
      /* poll() only reports that the attempt finished. Whether it succeeded is
       * kept in the pending socket error. */
      int error = 0;
      socklen_t len = sizeof(error);
      connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 &&
                  error == 0;
    }
  }

  if (!connected) {
    ::close(fd);
    return false;
  }

  if (::fcntl(fd, F_SETFL, flags) < 0) {
    ::close(fd);
    return false;
  }

  m_socket = fd;
  return true;
}

} // namespace gr::difi

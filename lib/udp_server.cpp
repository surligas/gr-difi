/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "udp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace gr::difi {

namespace {
/**
 * @brief Annotates a failure with the reason reported by the operating system.
 *
 * @param what description of the operation that failed
 * @return the description followed by the current errno string
 */
std::string errno_msg(const std::string &what) {
  return what + ": " + std::strerror(errno);
}
} // namespace

/**
 * @brief Creates a UDP server that listens on the specified IP address and
 * port.
 *
 * @param ip_addr the IP address to bind the server to
 * @param port the port number to listen on
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
udp_server::udp_server(std::string ip_addr, uint16_t port,
                       size_t recv_buffer_size, size_t send_buffer_size)
    : transport(recv_buffer_size, send_buffer_size), m_socket(-1) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_addr.c_str(), &addr) != 1) {
    throw std::invalid_argument("Not a valid IPv4 address: " + ip_addr);
  }
  bind_socket(addr.s_addr, port);
}

/**
 * @brief Creates a UDP server that listens on the specified port and binds to
 * all available network interfaces.
 *
 * @param port the port number to listen on
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
udp_server::udp_server(uint16_t port, size_t recv_buffer_size,
                       size_t send_buffer_size)
    : transport(recv_buffer_size, send_buffer_size), m_socket(-1) {
  bind_socket(INADDR_ANY, port);
}

udp_server::~udp_server() {
  if (m_socket >= 0) {
    ::close(m_socket);
  }
}

/**
 * @brief Opens the datagram socket and binds it to a local endpoint.
 *
 * @param addr the local address to bind to, in network byte order
 * @param port the port number to listen on
 */
void udp_server::bind_socket(in_addr_t addr, uint16_t port) {
  std::memset(&m_peer, 0, sizeof(m_peer));
  m_peer_len = 0;

  m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (m_socket < 0) {
    throw std::runtime_error(errno_msg("Could not create the UDP socket"));
  }

  /* The object is not fully constructed until this returns, so its destructor
   * will not run and the socket has to be released by hand on failure. */
  try {
    const int recv_buf_size = static_cast<int>(m_recv_buffer_size);
    if (::setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &recv_buf_size,
                     sizeof(recv_buf_size)) < 0) {
      throw std::runtime_error(
          errno_msg("Could not set the socket receive buffer size"));
    }

    const int send_buf_size = static_cast<int>(m_send_buffer_size);
    if (::setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size,
                     sizeof(send_buf_size)) < 0) {
      throw std::runtime_error(
          errno_msg("Could not set the socket send buffer size"));
    }

    struct timeval tv = {};
    tv.tv_usec = RECV_TIMEOUT_US;
    if (::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      throw std::runtime_error(
          errno_msg("Could not set the socket receive timeout"));
    }

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = addr;

    if (::bind(m_socket, reinterpret_cast<const struct sockaddr *>(&local),
               sizeof(local)) < 0) {
      throw std::runtime_error(errno_msg("Could not bind to port " +
                                         std::to_string(port) +
                                         ", it may already be in use"));
    }
  } catch (...) {
    ::close(m_socket);
    m_socket = -1;
    throw;
  }
}

/**
 * @brief Sends a datagram to the peer that transmitted to this server most
 * recently.
 *
 * @param data buffer containing the data to send
 * @param len the number of bytes to send from the buffer
 */
void udp_server::send(const void *data, size_t len) {
  if (m_peer_len == 0) {
    throw std::runtime_error(
        "Cannot send before a datagram has been received from a peer");
  }

  ssize_t nbytes =
      ::sendto(m_socket, data, len, 0,
               reinterpret_cast<const struct sockaddr *>(&m_peer), m_peer_len);
  if (nbytes < 0) {
    throw std::runtime_error(errno_msg("Could not send the datagram"));
  }
  if (static_cast<size_t>(nbytes) != len) {
    throw std::runtime_error("Sent only " + std::to_string(nbytes) + " of " +
                             std::to_string(len) + " bytes");
  }
}

/**
 * @brief Receives a single datagram and records its sender.
 *
 * Blocks until a datagram arrives or the receive timeout expires, whichever
 * comes first.
 *
 * @param buf buffer to store the received data
 * @param len the capacity of the buffer in bytes
 * @return the number of bytes received, or the negated errno on failure. A
 * receive timeout is reported as -EAGAIN
 */
ssize_t udp_server::recv(void *buf, size_t len) {
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);

  ssize_t nbytes = ::recvfrom(m_socket, buf, len, 0,
                              reinterpret_cast<struct sockaddr *>(&peer),
                              &peer_len);
  if (nbytes < 0) {
    return -errno;
  }

  /* Committed only on success, so that a failed receive leaves the previously
   * known peer intact. */
  m_peer = peer;
  m_peer_len = peer_len;
  return nbytes;
}

} // namespace gr::difi

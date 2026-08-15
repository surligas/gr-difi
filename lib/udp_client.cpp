/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "udp_client.hpp"

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
 * @brief Creates a UDP client bound to the given remote endpoint.
 *
 * @param ip_addr the IP address of the remote endpoint
 * @param port the port number of the remote endpoint
 * @param recv_buffer_size the size in bytes of the socket receive buffer
 * @param send_buffer_size the size in bytes of the socket send buffer
 */
udp_client::udp_client(std::string ip_addr, uint16_t port,
                       size_t recv_buffer_size, size_t send_buffer_size)
    : transport(recv_buffer_size, send_buffer_size), m_socket(-1) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_addr.c_str(), &addr) != 1) {
    throw std::invalid_argument("Not a valid IPv4 address: " + ip_addr);
  }
  connect_socket(addr.s_addr, port);
}

udp_client::~udp_client() {
  if (m_socket >= 0) {
    ::close(m_socket);
  }
}

/**
 * @brief Opens the datagram socket and associates it with the remote endpoint.
 *
 * Connecting a datagram socket does not send anything over the network. It sets
 * the destination of every later send, and tells the kernel to drop datagrams
 * that arrive from any other address.
 *
 * @param addr the remote address to connect to, in network byte order
 * @param port the remote port number
 */
void udp_client::connect_socket(in_addr_t addr, uint16_t port) {
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

    struct sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = addr;

    if (::connect(m_socket, reinterpret_cast<const struct sockaddr *>(&remote),
                  sizeof(remote)) < 0) {
      throw std::runtime_error(errno_msg("Could not connect to port " +
                                         std::to_string(port)));
    }
  } catch (...) {
    ::close(m_socket);
    m_socket = -1;
    throw;
  }
}

/**
 * @brief Sends a datagram to the remote endpoint.
 *
 * @param data buffer containing the data to send
 * @param len the number of bytes to send from the buffer
 */
void udp_client::send(const void *data, size_t len) {
  ssize_t nbytes = ::send(m_socket, data, len, 0);
  if (nbytes < 0) {
    /* When nothing listens on the remote port, the peer answers with an ICMP
     * port unreachable message. On a connected socket the kernel reports it as
     * an error on the next call, not on the send that caused it. The earlier
     * datagram was still transmitted, and the receiver may start at any moment,
     * so this error must not stop a flowgraph that is already running. */
    if (errno == ECONNREFUSED) {
      return;
    }
    throw std::runtime_error(errno_msg("Could not send the datagram"));
  }
  if (static_cast<size_t>(nbytes) != len) {
    throw std::runtime_error("Sent only " + std::to_string(nbytes) + " of " +
                             std::to_string(len) + " bytes");
  }
}

/**
 * @brief Receives a single datagram from the remote endpoint.
 *
 * Blocks until a datagram arrives or the receive timeout expires, whichever
 * comes first. The kernel drops datagrams sent by any other peer, so they never
 * reach this call.
 *
 * @param buf buffer to store the received data
 * @param len the capacity of the buffer in bytes
 * @return the number of bytes received, or the negated errno on failure. A
 * receive timeout is reported as -EAGAIN, and a peer that is not listening as
 * -ECONNREFUSED
 */
ssize_t udp_client::recv(void *buf, size_t len) {
  ssize_t nbytes = ::recv(m_socket, buf, len, 0);
  if (nbytes < 0) {
    return -errno;
  }
  return nbytes;
}

} // namespace gr::difi

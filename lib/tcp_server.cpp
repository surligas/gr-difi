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
#include <cstring>
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
    : transport(recv_buffer_size, send_buffer_size), m_listener(-1),
      m_client(-1) {
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
    : transport(recv_buffer_size, send_buffer_size), m_listener(-1),
      m_client(-1) {
  listen_socket(INADDR_ANY, port);
}

tcp_server::~tcp_server() {
  if (m_client >= 0) {
    ::close(m_client);
  }
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
bool tcp_server::accept_client() {
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

  m_client = client;
  return true;
}

void tcp_server::drop_client() {
  if (m_client >= 0) {
    ::close(m_client);
    m_client = -1;
  }
}

/**
 * @brief Reads exactly \p len bytes from the connected peer.
 *
 * A receive timeout is only reported to the caller when no byte of the current
 * packet has been read yet. Once a packet has started, this keeps waiting until
 * the packet is complete, because returning early would leave the unread
 * remainder in the stream and every later packet would be misaligned.
 *
 * @param buf buffer to store the received data
 * @param len the number of bytes to read
 * @return \p len, or the negated errno on failure
 */
ssize_t tcp_server::read_fully(void *buf, size_t len) {
  uint8_t *out = static_cast<uint8_t *>(buf);
  size_t total = 0;

  while (total < len) {
    ssize_t nbytes = ::recv(m_client, out + total, len - total, 0);
    if (nbytes == 0) {
      drop_client();
      return -ECONNRESET;
    }
    if (nbytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (total == 0) {
          return -EAGAIN;
        }
        continue;
      }
      int err = errno;
      drop_client();
      return -err;
    }
    total += nbytes;
  }
  return static_cast<ssize_t>(total);
}

/**
 * @brief Sends data to the connected peer.
 *
 * @param data buffer containing the data to send
 * @param len the number of bytes to send from the buffer
 */
void tcp_server::send(const void *data, size_t len) {
  if (m_client < 0) {
    throw std::runtime_error("Cannot send before a peer has connected");
  }

  const uint8_t *in = static_cast<const uint8_t *>(data);
  size_t total = 0;

  while (total < len) {
    /* MSG_NOSIGNAL, because a peer that disappears must produce an error here
     * and not a SIGPIPE that terminates the whole process. */
    ssize_t nbytes = ::send(m_client, in + total, len - total, MSG_NOSIGNAL);
    if (nbytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(errno_msg("Could not send the data"));
    }
    total += nbytes;
  }
}

/**
 * @brief Receives exactly one DIFI packet from the connected peer.
 *
 * Accepts a peer first if none is connected yet. The size of the packet is
 * taken from bits 0 to 15 of the DIFI header, which count the length of the
 * packet in 32 bit words.
 *
 * @param buf buffer to store the received packet
 * @param len the capacity of the buffer in bytes
 * @return the size of the packet in bytes, or the negated errno on failure. No
 * peer connected and no data available are both reported as -EAGAIN. A peer
 * that disconnected is reported as -ECONNRESET, a packet larger than the buffer
 * as -EMSGSIZE, and a packet whose header states an impossible size as -EPROTO
 */
ssize_t tcp_server::recv(void *buf, size_t len) {
  if (m_client < 0 && !accept_client()) {
    return -EAGAIN;
  }

  if (len < WORD_SIZE) {
    return -EINVAL;
  }

  uint8_t *out = static_cast<uint8_t *>(buf);

  ssize_t ret = read_fully(out, WORD_SIZE);
  if (ret < 0) {
    return ret;
  }

  uint32_t header;
  std::memcpy(&header, out, WORD_SIZE);
  const size_t packet_size = WORD_SIZE * (ntohl(header) & 0xffff);

  /* The stream cannot be resynchronised once the size is unusable, because the
   * start of the next packet is unknown. Dropping the peer lets it reconnect
   * and start again from a packet boundary. */
  if (packet_size < WORD_SIZE) {
    drop_client();
    return -EPROTO;
  }
  if (packet_size > len) {
    drop_client();
    return -EMSGSIZE;
  }

  const size_t remaining = packet_size - WORD_SIZE;
  if (remaining > 0) {
    ret = read_fully(out + WORD_SIZE, remaining);
    if (ret < 0) {
      return ret;
    }
  }
  return static_cast<ssize_t>(packet_size);
}

} // namespace gr::difi

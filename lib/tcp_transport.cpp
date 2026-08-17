/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "tcp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace gr::difi {

tcp_transport::~tcp_transport() {
  if (m_socket >= 0) {
    ::close(m_socket);
  }
}

void tcp_transport::close_connection() {
  if (m_socket >= 0) {
    ::close(m_socket);
    m_socket = -1;
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
ssize_t tcp_transport::read_fully(void *buf, size_t len) {
  uint8_t *out = static_cast<uint8_t *>(buf);
  size_t total = 0;

  while (total < len) {
    ssize_t nbytes = ::recv(m_socket, out + total, len - total, 0);
    if (nbytes == 0) {
      close_connection();
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
      close_connection();
      return -err;
    }
    total += nbytes;
  }
  return static_cast<ssize_t>(total);
}

/**
 * @brief Sends data to the connected peer, connecting first when needed.
 *
 * @param data buffer containing the data to send
 * @param len the number of bytes to send from the buffer
 */
void tcp_transport::send(const void *data, size_t len) {
  if (m_socket < 0 && !ensure_connected()) {
    throw std::runtime_error("Cannot send while no peer is connected");
  }

  const uint8_t *in = static_cast<const uint8_t *>(data);
  size_t total = 0;

  while (total < len) {
    /* MSG_NOSIGNAL, because a peer that disappears must produce an error here
     * and not a SIGPIPE that terminates the whole process. */
    ssize_t nbytes = ::send(m_socket, in + total, len - total, MSG_NOSIGNAL);
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
 * @brief Receives exactly one DIFI packet, connecting first when needed.
 *
 * The size of the packet is taken from bits 0 to 15 of the DIFI header, which
 * count the length of the packet in 32 bit words.
 *
 * @param buf buffer to store the received packet
 * @param len the capacity of the buffer in bytes
 * @return the size of the packet in bytes, or the negated errno on failure. No
 * connection and no data available are both reported as -EAGAIN. A peer that
 * disconnected is reported as -ECONNRESET, a packet larger than the buffer as
 * -EMSGSIZE, and a packet whose header states an impossible size as -EPROTO
 */
ssize_t tcp_transport::recv(void *buf, size_t len) {
  if (m_socket < 0 && !ensure_connected()) {
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
   * start of the next packet is unknown. Dropping the connection lets the peer
   * reconnect and start again from a packet boundary. */
  if (packet_size < WORD_SIZE) {
    close_connection();
    return -EPROTO;
  }
  if (packet_size > len) {
    close_connection();
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

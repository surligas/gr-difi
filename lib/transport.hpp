/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <cstddef>
#include <difi/api.h>
#include <sys/types.h>

namespace gr::difi {

/**
 * @brief Generic transport interface for sending and receiving data over
 * different transport layers (e.g., TCP, UDP).
 *
 */
class DIFI_API transport {
public:
  /* Large enough to absorb a burst of traffic while the flowgraph is busy
   * elsewhere. The operating system may clamp a request to a smaller value. */
  static constexpr size_t DEFAULT_BUFFER_SIZE = 2000000;

  /**
   * @brief Constructs the transport with the socket buffer sizes that derived
   * classes should request from the operating system.
   *
   * @param recv_buffer_size the size in bytes of the receive buffer
   * @param send_buffer_size the size in bytes of the send buffer
   */
  transport(size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
            size_t send_buffer_size = DEFAULT_BUFFER_SIZE)
      : m_recv_buffer_size(recv_buffer_size),
        m_send_buffer_size(send_buffer_size) {}

  /* Virtual, so that deleting a concrete transport through a transport
   * pointer runs the derived destructor and releases the socket. */
  virtual ~transport() = default;

  /**
   * @brief Sends data over the transport layer.
   *
   * @param data buffer containing the data to send
   * @param len the number of bytes to send from the buffer
   */
  virtual void send(const void *data, size_t len) = 0;

  /**
   * @brief Receives data over the transport layer.
   *
   * @param buf buffer to store the received data
   * @param len the number of bytes to receive into the buffer
   * @return the number of bytes received, or negative error code on failure
   */
  virtual ssize_t recv(void *buf, size_t len) = 0;

protected:
  const size_t m_recv_buffer_size;
  const size_t m_send_buffer_size;
};
} // namespace gr::difi
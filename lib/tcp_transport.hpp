/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include "transport.hpp"
#include <cstddef>

namespace gr::difi {

/**
 * @brief The behaviour that the TCP transports have in common.
 *
 * TCP delivers a stream of bytes and does not keep the boundaries between the
 * packets that were written into it. recv() restores those boundaries. It reads
 * the packet size from the DIFI header and then reads exactly one packet, so
 * the caller receives the same units that a UDP transport would deliver and
 * does not need to know which protocol is in use.
 *
 * A derived class only describes how a connection comes into existence. The
 * server waits for a peer to arrive and the client reaches out to one.
 */
class DIFI_API tcp_transport : public transport {
public:
  ~tcp_transport();

  void send(const void *data, size_t len) override;
  ssize_t recv(void *buf, size_t len) override;

protected:
  /* A DIFI header counts the size of a packet in 32 bit words. */
  static constexpr size_t WORD_SIZE = 4;

  tcp_transport(size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
                size_t send_buffer_size = DEFAULT_BUFFER_SIZE)
      : transport(recv_buffer_size, send_buffer_size) {}

  /**
   * @brief Establishes the connection when there is none.
   *
   * Called by send() and recv() before they use the socket. An implementation
   * must not block longer than RECV_TIMEOUT_US, so that a caller polling this
   * transport can still react to a shutdown request.
   *
   * @return true if a connection is available afterwards
   */
  virtual bool ensure_connected() = 0;

  /*! \brief Closes the connection, leaving the transport ready to reconnect. */
  void close_connection();

  /* The connected socket, or -1 while there is no connection. */
  int m_socket = -1;

private:
  ssize_t read_fully(void *buf, size_t len);
};
} // namespace gr::difi

/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include "transport.hpp"
#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace gr::difi {

/**
 * @brief A TCP transport that waits for one remote peer to connect.
 *
 * TCP delivers a stream of bytes and does not preserve the boundaries of the
 * packets that were written into it. recv() restores those boundaries. It reads
 * the packet size from the DIFI header and then reads exactly one packet, so
 * the caller receives the same units that a UDP transport would deliver and
 * does not need to know which protocol is in use.
 *
 * Only one peer is served at a time. When that peer disconnects, the next one
 * to arrive is accepted.
 */
class DIFI_API tcp_server : public transport {
public:
  tcp_server(std::string ip_addr, uint16_t port,
             size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  tcp_server(uint16_t port, size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  ~tcp_server();

  tcp_server(const tcp_server &) = delete;
  tcp_server &operator=(const tcp_server &) = delete;

  void send(const void *data, size_t len) override;
  ssize_t recv(void *buf, size_t len) override;

private:
  /* A DIFI header counts the size of a packet in 32 bit words. */
  static constexpr size_t WORD_SIZE = 4;

  void listen_socket(in_addr_t addr, uint16_t port);
  bool accept_client();
  void drop_client();
  ssize_t read_fully(void *buf, size_t len);

  int m_listener;
  int m_client;
};
} // namespace gr::difi

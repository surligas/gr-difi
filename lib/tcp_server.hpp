/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include "tcp_transport.hpp"
#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace gr::difi {

/**
 * @brief A TCP transport that waits for one remote peer to connect.
 *
 * Only one peer is served at a time. When that peer disconnects, the next one
 * to arrive is accepted. See tcp_transport for how packets are read from the
 * stream.
 */
class DIFI_API tcp_server : public tcp_transport {
public:
  tcp_server(std::string ip_addr, uint16_t port,
             size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  tcp_server(uint16_t port, size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  ~tcp_server();

  tcp_server(const tcp_server &) = delete;
  tcp_server &operator=(const tcp_server &) = delete;

protected:
  bool ensure_connected() override;

private:
  void listen_socket(in_addr_t addr, uint16_t port);

  int m_listener;
};
} // namespace gr::difi

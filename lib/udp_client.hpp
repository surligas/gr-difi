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
 * @brief A UDP transport that talks to a fixed remote endpoint.
 *
 * The socket is connected to that endpoint, so send() does not need a
 * destination address and the kernel drops datagrams that arrive from any other
 * address. This is the counterpart of udp_server, which binds a local endpoint
 * and accepts datagrams from every peer.
 */
class DIFI_API udp_client : public transport {
public:
  udp_client(std::string ip_addr, uint16_t port,
             size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  ~udp_client();

  udp_client(const udp_client &) = delete;
  udp_client &operator=(const udp_client &) = delete;

  void send(const void *data, size_t len) override;
  ssize_t recv(void *buf, size_t len) override;

private:
  void connect_socket(in_addr_t addr, uint16_t port);

  int m_socket;
};
} // namespace gr::difi

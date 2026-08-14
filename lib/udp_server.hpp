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
 * @brief A UDP transport bound to a local endpoint, accepting datagrams from
 * any peer.
 *
 * Every successful recv() records the sender of the datagram, so send()
 * replies to whichever peer transmitted most recently.
 */
class DIFI_API udp_server : public transport {
public:
  udp_server(std::string ip_addr, uint16_t port,
             size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  udp_server(uint16_t port, size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  ~udp_server();

  udp_server(const udp_server &) = delete;
  udp_server &operator=(const udp_server &) = delete;

  void send(const void *data, size_t len) override;
  ssize_t recv(void *buf, size_t len) override;

private:
  /* Caps how long recv() blocks, so that a caller polling this transport can
   * still react to a shutdown request when no traffic arrives. */
  static constexpr long RECV_TIMEOUT_US = 100000;

  void bind_socket(in_addr_t addr, uint16_t port);

  int m_socket;
  struct sockaddr_in m_peer;
  socklen_t m_peer_len;
};
} // namespace gr::difi

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
 * @brief A TCP transport that connects to a fixed remote endpoint.
 *
 * The constructor does not connect. The connection is made when the transport
 * is first used, and again after it is lost, because the peer is often started
 * later than the flowgraph. See tcp_transport for how packets are read from the
 * stream.
 */
class DIFI_API tcp_client : public tcp_transport {
public:
  tcp_client(std::string ip_addr, uint16_t port,
             size_t recv_buffer_size = DEFAULT_BUFFER_SIZE,
             size_t send_buffer_size = DEFAULT_BUFFER_SIZE);

  tcp_client(const tcp_client &) = delete;
  tcp_client &operator=(const tcp_client &) = delete;

protected:
  bool ensure_connected() override;

private:
  struct sockaddr_in m_remote;
};
} // namespace gr::difi

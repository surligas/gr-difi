/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "transport.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace gr::difi {

std::string transport::errno_msg(const std::string &what) {
  return what + ": " + std::strerror(errno);
}

void transport::apply_socket_options(int fd) const {
  const int recv_buf_size = static_cast<int>(m_recv_buffer_size);
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_buf_size,
                   sizeof(recv_buf_size)) < 0) {
    throw std::runtime_error(
        errno_msg("Could not set the socket receive buffer size"));
  }

  const int send_buf_size = static_cast<int>(m_send_buffer_size);
  if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buf_size,
                   sizeof(send_buf_size)) < 0) {
    throw std::runtime_error(
        errno_msg("Could not set the socket send buffer size"));
  }

  struct timeval tv = {};
  tv.tv_usec = RECV_TIMEOUT_US;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    throw std::runtime_error(
        errno_msg("Could not set the socket receive timeout"));
  }
}

int transport::create_socket(int type, int protocol) const {
  int fd = ::socket(AF_INET, type, protocol);
  if (fd < 0) {
    throw std::runtime_error(errno_msg("Could not create the socket"));
  }

  try {
    apply_socket_options(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  return fd;
}

} // namespace gr::difi

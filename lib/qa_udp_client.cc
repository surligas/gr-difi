/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "qa_transport_utils.hpp"
#include "udp_client.hpp"

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>

using namespace gr::difi;
using namespace gr::difi::qa;

BOOST_AUTO_TEST_CASE(t_send_reaches_the_peer)
{
    uint16_t port = free_port();
    peer remote(port);
    udp_client client(LOOPBACK, port);

    client.send("hello difi", 10);

    BOOST_CHECK_EQUAL(remote.recv(), "hello difi");
}

BOOST_AUTO_TEST_CASE(t_recv_returns_the_reply)
{
    uint16_t port = free_port();
    peer remote(port);
    udp_client client(LOOPBACK, port);

    client.send("ping", 4);
    BOOST_REQUIRE_EQUAL(remote.recv(), "ping");
    remote.reply("pong");

    char buf[64] = {};
    ssize_t n = client.recv(buf, sizeof(buf));

    BOOST_CHECK_EQUAL(n, 4);
    BOOST_CHECK_EQUAL(std::string(buf, n), "pong");
}

BOOST_AUTO_TEST_CASE(t_recv_ignores_datagrams_from_other_sources)
{
    uint16_t port = free_port();
    peer remote(port);

    std::unique_ptr<udp_client> client;
    int fd = fd_opened_by([&] { client = std::make_unique<udp_client>(LOOPBACK, port); });
    BOOST_REQUIRE(fd >= 0);

    // Another peer sends directly to the port of the client. The socket is
    // connected, so the kernel drops this datagram before it can be read.
    peer stranger;
    stranger.send_to(local_port(fd), "spoofed");

    char buf[64] = {};
    ssize_t n = client->recv(buf, sizeof(buf));
    BOOST_CHECK(n == -EAGAIN || n == -EWOULDBLOCK);

    // The configured endpoint is still received.
    client->send("ping", 4);
    BOOST_REQUIRE_EQUAL(remote.recv(), "ping");
    remote.reply("pong");

    BOOST_CHECK_EQUAL(client->recv(buf, sizeof(buf)), 4);
}

BOOST_AUTO_TEST_CASE(t_recv_times_out_rather_than_blocking)
{
    uint16_t port = free_port();
    // The peer is bound but never answers. It exists so that the port stays
    // open and the kernel does not report the port as unreachable.
    peer remote(port);
    udp_client client(LOOPBACK, port);

    auto start = std::chrono::steady_clock::now();
    char buf[64];
    ssize_t n = client.recv(buf, sizeof(buf));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    BOOST_CHECK(n == -EAGAIN || n == -EWOULDBLOCK);
    BOOST_CHECK_GE(elapsed, 90);
    BOOST_CHECK_LT(elapsed, 1000);
}

BOOST_AUTO_TEST_CASE(t_send_to_a_closed_port_does_not_throw)
{
    // No socket listens on this port, so the first datagram causes an ICMP port
    // unreachable message that the kernel reports on a later call. A sink must
    // keep sending until its receiver is started.
    udp_client client(LOOPBACK, free_port());

    for (int i = 0; i < 5; i++) {
        BOOST_CHECK_NO_THROW(client.send("data", 4));
    }
}

BOOST_AUTO_TEST_CASE(t_invalid_address_is_rejected)
{
    BOOST_CHECK_THROW(udp_client("not.an.ip", free_port()), std::invalid_argument);
    BOOST_CHECK_THROW(udp_client("", free_port()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(t_destructor_closes_the_socket)
{
    uint16_t port = free_port();

    // The client must stay alive after the lambda returns. If it were destroyed
    // inside the lambda, its socket would already be closed and fd_opened_by()
    // would find no new descriptor.
    std::unique_ptr<udp_client> client;
    int fd = fd_opened_by([&] { client = std::make_unique<udp_client>(LOOPBACK, port); });
    BOOST_REQUIRE(fd >= 0);

    client.reset();

    BOOST_CHECK(::fcntl(fd, F_GETFD) == -1);
}

BOOST_AUTO_TEST_CASE(t_deleting_through_the_base_closes_the_socket)
{
    uint16_t port = free_port();

    std::unique_ptr<transport> t;
    int fd = fd_opened_by([&] { t = std::make_unique<udp_client>(LOOPBACK, port); });
    BOOST_REQUIRE(fd >= 0);

    t.reset();

    // Fails if ~transport is not virtual, since ~udp_client would never run.
    BOOST_CHECK(::fcntl(fd, F_GETFD) == -1);
}

BOOST_AUTO_TEST_CASE(t_buffer_sizes_reach_the_socket)
{
    uint16_t port = free_port();
    const size_t requested = 512000;

    std::unique_ptr<udp_client> client;
    int fd = fd_opened_by([&] {
        client = std::make_unique<udp_client>(LOOPBACK, port, requested, requested);
    });
    BOOST_REQUIRE(fd >= 0);

    // Linux reports back at least what was asked for, usually double.
    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)requested);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)requested);
}

BOOST_AUTO_TEST_CASE(t_omitted_buffer_sizes_use_the_default)
{
    uint16_t port = free_port();

    std::unique_ptr<udp_client> client;
    int fd = fd_opened_by([&] { client = std::make_unique<udp_client>(LOOPBACK, port); });
    BOOST_REQUIRE(fd >= 0);

    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)transport::DEFAULT_BUFFER_SIZE);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)transport::DEFAULT_BUFFER_SIZE);
}

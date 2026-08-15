/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "qa_transport_utils.hpp"
#include "udp_server.hpp"

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

using namespace gr::difi;
using namespace gr::difi::qa;

BOOST_AUTO_TEST_CASE(t_recv_returns_the_datagram)
{
    uint16_t port = free_port();
    udp_server server(LOOPBACK, port);
    peer client;

    client.send_to(port, "hello difi");

    char buf[64] = {};
    ssize_t n = server.recv(buf, sizeof(buf));

    BOOST_CHECK_EQUAL(n, 10);
    BOOST_CHECK_EQUAL(std::string(buf, n), "hello difi");
}

BOOST_AUTO_TEST_CASE(t_send_replies_to_the_last_peer)
{
    uint16_t port = free_port();
    udp_server server(LOOPBACK, port);
    peer client;

    client.send_to(port, "ping");
    char buf[64];
    server.recv(buf, sizeof(buf));

    server.send("pong", 4);

    BOOST_CHECK_EQUAL(client.recv(), "pong");
}

BOOST_AUTO_TEST_CASE(t_send_without_a_peer_throws)
{
    udp_server server(free_port());

    // Nothing has been received yet, so there is no address to reply to.
    BOOST_CHECK_THROW(server.send("data", 4), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_recv_times_out_rather_than_blocking)
{
    udp_server server(free_port());

    auto start = std::chrono::steady_clock::now();
    char buf[64];
    ssize_t n = server.recv(buf, sizeof(buf));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    BOOST_CHECK(n == -EAGAIN || n == -EWOULDBLOCK);
    BOOST_CHECK_GE(elapsed, 90);
    BOOST_CHECK_LT(elapsed, 1000);
}

BOOST_AUTO_TEST_CASE(t_timed_out_recv_keeps_the_known_peer)
{
    uint16_t port = free_port();
    udp_server server(LOOPBACK, port);
    peer client;

    client.send_to(port, "ping");
    char buf[64];
    server.recv(buf, sizeof(buf));
    server.recv(buf, sizeof(buf)); // times out, must not forget the peer

    server.send("pong", 4);

    BOOST_CHECK_EQUAL(client.recv(), "pong");
}

BOOST_AUTO_TEST_CASE(t_any_interface_ctor_accepts_traffic_on_loopback)
{
    uint16_t port = free_port();
    udp_server server(port);
    peer client;

    client.send_to(port, "hello");

    char buf[64] = {};
    BOOST_CHECK_EQUAL(server.recv(buf, sizeof(buf)), 5);
}

BOOST_AUTO_TEST_CASE(t_invalid_address_is_rejected)
{
    BOOST_CHECK_THROW(udp_server("not.an.ip", free_port()), std::invalid_argument);
    BOOST_CHECK_THROW(udp_server("", free_port()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(t_binding_a_busy_port_throws)
{
    uint16_t port = free_port();
    udp_server first(port);

    BOOST_CHECK_THROW(udp_server{ port }, std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_destructor_releases_the_port)
{
    uint16_t port = free_port();
    { udp_server server(port); }

    // Rebinding only succeeds if the socket was closed.
    BOOST_CHECK_NO_THROW(udp_server{ port });
}

BOOST_AUTO_TEST_CASE(t_deleting_through_the_base_releases_the_port)
{
    uint16_t port = free_port();
    { std::unique_ptr<transport> t = std::make_unique<udp_server>(port); }

    // Fails if ~transport is not virtual, since ~udp_server would never run.
    BOOST_CHECK_NO_THROW(udp_server{ port });
}

BOOST_AUTO_TEST_CASE(t_buffer_sizes_reach_the_socket)
{
    uint16_t port = free_port();
    const size_t requested = 512000;

    std::unique_ptr<udp_server> server;
    int fd = fd_opened_by(
        [&] { server = std::make_unique<udp_server>(port, requested, requested); });
    BOOST_REQUIRE(fd >= 0);

    // Linux reports back at least what was asked for, usually double.
    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)requested);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)requested);
}

BOOST_AUTO_TEST_CASE(t_omitted_buffer_sizes_use_the_default)
{
    uint16_t port = free_port();

    std::unique_ptr<udp_server> server;
    int fd = fd_opened_by([&] { server = std::make_unique<udp_server>(port); });
    BOOST_REQUIRE(fd >= 0);

    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)transport::DEFAULT_BUFFER_SIZE);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)transport::DEFAULT_BUFFER_SIZE);
}

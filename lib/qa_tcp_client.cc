/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "qa_transport_utils.hpp"
#include "tcp_client.hpp"

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>

using namespace gr::difi;
using namespace gr::difi::qa;

namespace {

/*!
 * \brief Connects \p client to \p listener by sending one packet through it.
 *
 * The client only connects when it is first used, so a test that needs an
 * established connection has to drive one call through it first.
 */
void establish(tcp_client &client, tcp_listener &listener)
{
    const std::string packet = difi_packet(28, 'i');
    std::thread accepting([&] { BOOST_REQUIRE(listener.accept_peer()); });
    client.send(packet.data(), packet.size());
    accepting.join();
    BOOST_REQUIRE_EQUAL(listener.recv(28).size(), 28u);
}

} // namespace

BOOST_AUTO_TEST_CASE(t_constructor_does_not_connect)
{
    // The sink is often started before the receiver it feeds, so a peer that is
    // not listening yet must not prevent the transport from being created.
    BOOST_CHECK_NO_THROW(tcp_client(LOOPBACK, free_port()));
}

BOOST_AUTO_TEST_CASE(t_invalid_address_is_rejected)
{
    BOOST_CHECK_THROW(tcp_client("not.an.ip", free_port()), std::invalid_argument);
    BOOST_CHECK_THROW(tcp_client("", free_port()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(t_send_connects_on_demand)
{
    uint16_t port = free_port();
    tcp_listener listener(port);
    tcp_client client(LOOPBACK, port);

    const std::string packet = difi_packet(28, 'q');
    std::thread accepting([&] { BOOST_REQUIRE(listener.accept_peer()); });
    client.send(packet.data(), packet.size());
    accepting.join();

    std::string got = listener.recv(28);
    BOOST_CHECK_EQUAL(got.size(), 28u);
    BOOST_CHECK_EQUAL(got[27], 'q');
}

BOOST_AUTO_TEST_CASE(t_send_without_a_peer_throws)
{
    tcp_client client(LOOPBACK, free_port());

    BOOST_CHECK_THROW(client.send("data", 4), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_recv_without_a_peer_reports_no_data)
{
    tcp_client client(LOOPBACK, free_port());

    auto start = std::chrono::steady_clock::now();
    char buf[64];
    ssize_t n = client.recv(buf, sizeof(buf));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    BOOST_CHECK(n == -EAGAIN || n == -EWOULDBLOCK);
    BOOST_CHECK_LT(elapsed, 1000);
}

BOOST_AUTO_TEST_CASE(t_recv_returns_one_whole_packet)
{
    uint16_t port = free_port();
    tcp_listener listener(port);
    tcp_client client(LOOPBACK, port);
    establish(client, listener);

    listener.send(difi_packet(60, 'p'));

    char buf[256];
    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), 60);
    BOOST_CHECK_EQUAL(buf[59], 'p');
}

BOOST_AUTO_TEST_CASE(t_recv_splits_two_packets_sent_as_one_write)
{
    uint16_t port = free_port();
    tcp_listener listener(port);
    tcp_client client(LOOPBACK, port);
    establish(client, listener);

    listener.send(difi_packet(28, 'a') + difi_packet(60, 'b'));

    char buf[256];
    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), 28);
    BOOST_CHECK_EQUAL(buf[27], 'a');

    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), 60);
    BOOST_CHECK_EQUAL(buf[59], 'b');
}

BOOST_AUTO_TEST_CASE(t_disconnected_peer_is_reported)
{
    uint16_t port = free_port();
    tcp_listener listener(port);
    tcp_client client(LOOPBACK, port);
    establish(client, listener);

    listener.close_peer();

    char buf[256];
    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), -ECONNRESET);
}

BOOST_AUTO_TEST_CASE(t_reconnects_after_the_peer_comes_back)
{
    // The behaviour the sink depends on. A receiver that restarts must not
    // require the flowgraph to be restarted with it.
    uint16_t port = free_port();
    tcp_client client(LOOPBACK, port);
    char buf[256];

    {
        tcp_listener listener(port);
        establish(client, listener);
        listener.close_peer();
        BOOST_REQUIRE_EQUAL(client.recv(buf, sizeof(buf)), -ECONNRESET);
    }

    tcp_listener restarted(port);
    const std::string packet = difi_packet(28, 'r');
    std::thread accepting([&] { BOOST_REQUIRE(restarted.accept_peer()); });
    client.send(packet.data(), packet.size());
    accepting.join();

    BOOST_CHECK_EQUAL(restarted.recv(28).size(), 28u);

    restarted.send(difi_packet(40, 's'));
    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), 40);
    BOOST_CHECK_EQUAL(buf[39], 's');
}

BOOST_AUTO_TEST_CASE(t_packet_larger_than_the_buffer_is_rejected)
{
    uint16_t port = free_port();
    tcp_listener listener(port);
    tcp_client client(LOOPBACK, port);
    establish(client, listener);

    listener.send(difi_packet(120));

    char buf[64];
    BOOST_CHECK_EQUAL(client.recv(buf, sizeof(buf)), -EMSGSIZE);
}

BOOST_AUTO_TEST_CASE(t_destructor_closes_the_socket)
{
    uint16_t port = free_port();
    tcp_listener listener(port);

    auto client = std::make_unique<tcp_client>(LOOPBACK, port);
    int fd = fd_opened_by([&] { establish(*client, listener); });
    BOOST_REQUIRE(fd >= 0);

    client.reset();

    BOOST_CHECK(::fcntl(fd, F_GETFD) == -1);
}

BOOST_AUTO_TEST_CASE(t_buffer_sizes_reach_the_socket)
{
    uint16_t port = free_port();
    const size_t requested = 512000;
    tcp_listener listener(port);

    // The socket only exists once the transport connects, so the descriptor has
    // to be captured around the call that establishes the connection.
    tcp_client client(LOOPBACK, port, requested, requested);
    int fd = fd_opened_by([&] { establish(client, listener); });
    BOOST_REQUIRE(fd >= 0);

    // Linux reports back at least what was asked for, usually double.
    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)requested);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)requested);
}

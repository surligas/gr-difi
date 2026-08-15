/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "qa_transport_utils.hpp"
#include "tcp_server.hpp"

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>

using namespace gr::difi;
using namespace gr::difi::qa;

namespace {

/*!
 * \brief Calls recv() until it returns something other than a timeout.
 *
 * The first call after a peer connects can time out while accepting, so a test
 * that expects a packet has to allow for that without waiting forever.
 */
ssize_t recv_packet(tcp_server &server, char *buf, size_t len)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        ssize_t n = server.recv(buf, len);
        if (n != -EAGAIN && n != -EWOULDBLOCK) {
            return n;
        }
    }
    return -ETIMEDOUT;
}

} // namespace

BOOST_AUTO_TEST_CASE(t_recv_without_a_peer_times_out)
{
    tcp_server server(free_port());

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

BOOST_AUTO_TEST_CASE(t_recv_returns_one_whole_packet)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);
    client.send(difi_packet(28));

    char buf[256];
    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), 28);
}

BOOST_AUTO_TEST_CASE(t_recv_splits_two_packets_sent_as_one_write)
{
    // This is the reason the framing belongs in the transport. TCP does not
    // keep the boundary between the two packets, so recv() has to find it from
    // the size in the header.
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);
    client.send(difi_packet(28, 'a') + difi_packet(60, 'b'));

    char buf[256];
    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), 28);
    BOOST_CHECK_EQUAL(buf[27], 'a');

    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), 60);
    BOOST_CHECK_EQUAL(buf[59], 'b');
}

BOOST_AUTO_TEST_CASE(t_recv_waits_for_a_packet_split_across_writes)
{
    // The other half of the same problem: one packet arriving in several
    // pieces must not be reported before it is complete.
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);

    std::string packet = difi_packet(40, 'z');
    client.send(packet.substr(0, 6));

    std::thread rest([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        client.send(packet.substr(6));
    });

    char buf[256];
    ssize_t n = recv_packet(server, buf, sizeof(buf));
    rest.join();

    BOOST_CHECK_EQUAL(n, 40);
    BOOST_CHECK_EQUAL(buf[39], 'z');
}

BOOST_AUTO_TEST_CASE(t_disconnected_peer_is_reported)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);
    client.send(difi_packet(28));

    char buf[256];
    BOOST_REQUIRE_EQUAL(recv_packet(server, buf, sizeof(buf)), 28);

    client.close();

    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), -ECONNRESET);
}

BOOST_AUTO_TEST_CASE(t_another_peer_can_connect_after_the_first_left)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);
    char buf[256];

    {
        tcp_peer first;
        first.connect_to(port);
        first.send(difi_packet(28, 'a'));
        BOOST_REQUIRE_EQUAL(recv_packet(server, buf, sizeof(buf)), 28);
    }

    // The server notices the disconnect, then accepts the next peer.
    BOOST_REQUIRE_EQUAL(recv_packet(server, buf, sizeof(buf)), -ECONNRESET);

    tcp_peer second;
    second.connect_to(port);
    second.send(difi_packet(60, 'b'));

    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), 60);
    BOOST_CHECK_EQUAL(buf[59], 'b');
}

BOOST_AUTO_TEST_CASE(t_packet_larger_than_the_buffer_is_rejected)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);
    client.send(difi_packet(120));

    char buf[64];
    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), -EMSGSIZE);
}

BOOST_AUTO_TEST_CASE(t_impossible_packet_size_is_rejected)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);

    // A header claiming a length of zero words. The next packet could not be
    // located, so the peer is dropped instead.
    std::string bad(28, 'x');
    uint32_t header = htonl(0);
    std::memcpy(bad.data(), &header, sizeof(header));
    client.send(bad);

    char buf[256];
    BOOST_CHECK_EQUAL(recv_packet(server, buf, sizeof(buf)), -EPROTO);
}

BOOST_AUTO_TEST_CASE(t_send_without_a_peer_throws)
{
    tcp_server server(free_port());

    BOOST_CHECK_THROW(server.send("data", 4), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_send_reaches_the_peer)
{
    uint16_t port = free_port();
    tcp_server server(LOOPBACK, port);

    tcp_peer client;
    client.connect_to(port);

    // A packet has to arrive first, so that the server has accepted the peer.
    client.send(difi_packet(28));
    char buf[256];
    BOOST_REQUIRE_EQUAL(recv_packet(server, buf, sizeof(buf)), 28);

    server.send("pong", 4);

    BOOST_CHECK_EQUAL(client.recv(4), "pong");
}

BOOST_AUTO_TEST_CASE(t_invalid_address_is_rejected)
{
    BOOST_CHECK_THROW(tcp_server("not.an.ip", free_port()), std::invalid_argument);
    BOOST_CHECK_THROW(tcp_server("", free_port()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(t_binding_a_busy_port_throws)
{
    uint16_t port = free_port();
    tcp_server first(port);

    BOOST_CHECK_THROW(tcp_server{ port }, std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_destructor_releases_the_port)
{
    uint16_t port = free_port();
    { tcp_server server(port); }

    // Rebinding only succeeds if the listening socket was closed.
    BOOST_CHECK_NO_THROW(tcp_server{ port });
}

BOOST_AUTO_TEST_CASE(t_deleting_through_the_base_releases_the_port)
{
    uint16_t port = free_port();
    { std::unique_ptr<transport> t = std::make_unique<tcp_server>(port); }

    // Fails if ~transport is not virtual, since ~tcp_server would never run.
    BOOST_CHECK_NO_THROW(tcp_server{ port });
}

BOOST_AUTO_TEST_CASE(t_buffer_sizes_reach_the_listening_socket)
{
    uint16_t port = free_port();
    const size_t requested = 512000;

    std::unique_ptr<tcp_server> server;
    int fd = fd_opened_by(
        [&] { server = std::make_unique<tcp_server>(port, requested, requested); });
    BOOST_REQUIRE(fd >= 0);

    // Linux reports back at least what was asked for, usually double.
    BOOST_CHECK_GE(sock_opt(fd, SO_RCVBUF), (int)requested);
    BOOST_CHECK_GE(sock_opt(fd, SO_SNDBUF), (int)requested);
}

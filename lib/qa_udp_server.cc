/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "udp_server.hpp"

#include <arpa/inet.h>
#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace gr::difi;

namespace {

const char *LOOPBACK = "127.0.0.1";

/*!
 * \brief Asks the operating system for a port that is currently free.
 *
 * Mirrors get_open_ports() in python/qa_difi_blocks_cpp.py, so that concurrent
 * test runs do not collide on a hardcoded port.
 */
uint16_t free_port()
{
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOST_REQUIRE(s >= 0);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, LOOPBACK, &addr.sin_addr);
    BOOST_REQUIRE(::bind(s, (const sockaddr *)&addr, sizeof(addr)) == 0);

    sockaddr_in bound = {};
    socklen_t len = sizeof(bound);
    BOOST_REQUIRE(::getsockname(s, (sockaddr *)&bound, &len) == 0);
    ::close(s);

    return ntohs(bound.sin_port);
}

/*! \brief The set of file descriptors this process currently has open. */
std::set<int> open_fds()
{
    std::set<int> fds;
    DIR *dir = ::opendir("/proc/self/fd");
    BOOST_REQUIRE(dir != nullptr);

    // The directory handle occupies a descriptor that appears in its own
    // listing, and would otherwise show up as a spurious difference.
    const int self = ::dirfd(dir);

    while (dirent *entry = ::readdir(dir)) {
        try {
            int fd = std::stoi(entry->d_name);
            if (fd != self) {
                fds.insert(fd);
            }
        } catch (const std::exception &) {
            // "." and ".."
        }
    }
    ::closedir(dir);
    return fds;
}

/*!
 * \brief The descriptor opened by \p open, found by diffing the process file
 * descriptor table across the call.
 *
 * udp_server keeps its socket private, so this is how the tests reach it to
 * assert on the options it was configured with.
 */
template <typename F>
int fd_opened_by(F open)
{
    std::set<int> before = open_fds();
    open();
    for (int fd : open_fds()) {
        if (before.count(fd) == 0) {
            return fd;
        }
    }
    return -1;
}

/*! \brief A plain UDP socket standing in for a remote peer. */
class peer
{
public:
    peer() : m_socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))
    {
        BOOST_REQUIRE(m_socket >= 0);

        timeval tv = {};
        tv.tv_sec = 2;
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~peer() { ::close(m_socket); }

    void send_to(uint16_t port, const std::string &payload)
    {
        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        ::inet_pton(AF_INET, LOOPBACK, &dst.sin_addr);
        BOOST_REQUIRE(::sendto(m_socket,
                               payload.data(),
                               payload.size(),
                               0,
                               (const sockaddr *)&dst,
                               sizeof(dst)) == (ssize_t)payload.size());
    }

    std::string recv()
    {
        char buf[256] = {};
        ssize_t n = ::recv(m_socket, buf, sizeof(buf), 0);
        return n < 0 ? std::string() : std::string(buf, n);
    }

private:
    int m_socket;
};

int sock_opt(int fd, int optname)
{
    int value = 0;
    socklen_t len = sizeof(value);
    BOOST_REQUIRE(::getsockopt(fd, SOL_SOCKET, optname, &value, &len) == 0);
    return value;
}

} // namespace

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

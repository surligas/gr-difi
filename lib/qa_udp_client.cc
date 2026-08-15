/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "udp_client.hpp"

#include <arpa/inet.h>
#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
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
 * udp_client keeps its socket private, so this is how the tests reach it to
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

/*!
 * \brief A plain UDP socket standing in for the remote endpoint.
 *
 * Port 0 asks the kernel to choose a free port, which is enough for a peer that
 * only sends. Pass a port from free_port() when the client needs a known
 * destination address.
 */
class peer
{
public:
    peer() : peer(0) {}

    explicit peer(uint16_t port) : m_socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))
    {
        BOOST_REQUIRE(m_socket >= 0);

        timeval tv = {};
        tv.tv_sec = 2;
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        ::inet_pton(AF_INET, LOOPBACK, &local.sin_addr);
        BOOST_REQUIRE(::bind(m_socket, (const sockaddr *)&local, sizeof(local)) == 0);
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

    /*! \brief Receives a datagram, recording its sender so reply() can answer. */
    std::string recv()
    {
        char buf[256] = {};
        m_last_len = sizeof(m_last);
        ssize_t n = ::recvfrom(m_socket, buf, sizeof(buf), 0, (sockaddr *)&m_last, &m_last_len);
        if (n < 0) {
            m_last_len = 0;
            return std::string();
        }
        return std::string(buf, n);
    }

    void reply(const std::string &payload)
    {
        BOOST_REQUIRE(m_last_len != 0);
        BOOST_REQUIRE(::sendto(m_socket,
                               payload.data(),
                               payload.size(),
                               0,
                               (const sockaddr *)&m_last,
                               m_last_len) == (ssize_t)payload.size());
    }

private:
    int m_socket;
    sockaddr_in m_last = {};
    socklen_t m_last_len = 0;
};

int sock_opt(int fd, int optname)
{
    int value = 0;
    socklen_t len = sizeof(value);
    BOOST_REQUIRE(::getsockopt(fd, SOL_SOCKET, optname, &value, &len) == 0);
    return value;
}

/*! \brief The ephemeral port the kernel assigned to a connected socket. */
uint16_t local_port(int fd)
{
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, (sockaddr *)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

} // namespace

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

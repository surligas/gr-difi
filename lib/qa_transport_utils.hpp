/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <arpa/inet.h>
#include <boost/test/unit_test.hpp>
#include <cstring>
#include <dirent.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*!
 * \file
 * \brief Helpers shared by the transport test suites.
 *
 * Every test suite is compiled into its own binary, so these are inline
 * definitions in a header instead of a separate library.
 */

namespace gr::difi::qa {

const char *const LOOPBACK = "127.0.0.1";

/*!
 * \brief Asks the operating system for a port that is currently free.
 *
 * Mirrors get_open_ports() in python/qa_difi_blocks_cpp.py, so that concurrent
 * test runs do not collide on a hardcoded port.
 */
inline uint16_t free_port()
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
inline std::set<int> open_fds()
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
 * The transports keep their socket private, so this is how the tests reach it
 * to assert on the options it was configured with. The object must still be
 * alive when \p open returns, otherwise its socket is already closed and there
 * is no new descriptor left to find.
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
 * \brief A plain UDP socket standing in for a remote peer.
 *
 * Port 0 asks the kernel to choose a free port, which is enough for a peer that
 * only sends. Pass a port from free_port() when the transport under test needs
 * a known destination address.
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

    peer(const peer &) = delete;
    peer &operator=(const peer &) = delete;

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

    /*! \brief Sends a datagram back to the sender of the last received one. */
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

/*!
 * \brief A plain TCP socket standing in for the peer on the other end of a
 * stream transport.
 *
 * Connecting is a separate step from construction, so that a test can observe
 * how the transport behaves before any peer arrives.
 */
class tcp_peer
{
public:
    tcp_peer() : m_socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        BOOST_REQUIRE(m_socket >= 0);

        timeval tv = {};
        tv.tv_sec = 2;
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~tcp_peer() { close(); }

    tcp_peer(const tcp_peer &) = delete;
    tcp_peer &operator=(const tcp_peer &) = delete;

    void connect_to(uint16_t port)
    {
        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        ::inet_pton(AF_INET, LOOPBACK, &dst.sin_addr);
        BOOST_REQUIRE(::connect(m_socket, (const sockaddr *)&dst, sizeof(dst)) == 0);
    }

    void send(const std::string &payload)
    {
        size_t total = 0;
        while (total < payload.size()) {
            ssize_t n = ::send(
                m_socket, payload.data() + total, payload.size() - total, MSG_NOSIGNAL);
            BOOST_REQUIRE(n > 0);
            total += n;
        }
    }

    /*! \brief Reads up to \p len bytes, returning what arrived before the timeout. */
    std::string recv(size_t len)
    {
        std::string buf(len, '\0');
        ssize_t n = ::recv(m_socket, buf.data(), len, 0);
        return n < 0 ? std::string() : buf.substr(0, n);
    }

    /*! \brief Closes the connection, which the other end sees as end of stream. */
    void close()
    {
        if (m_socket >= 0) {
            ::close(m_socket);
            m_socket = -1;
        }
    }

private:
    int m_socket;
};

/*!
 * \brief A listening TCP socket standing in for the peer a client connects to.
 *
 * Accepting is a separate step, so that a test can control the moment the
 * connection is established.
 */
class tcp_listener
{
public:
    explicit tcp_listener(uint16_t port)
        : m_listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        BOOST_REQUIRE(m_listener >= 0);

        // Needed so that a test can bind the same port again after closing a
        // previous listener, while the old socket is still in TIME_WAIT.
        const int enable = 1;
        ::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        ::inet_pton(AF_INET, LOOPBACK, &local.sin_addr);
        BOOST_REQUIRE(::bind(m_listener, (const sockaddr *)&local, sizeof(local)) == 0);
        BOOST_REQUIRE(::listen(m_listener, 1) == 0);
    }

    ~tcp_listener()
    {
        close_peer();
        if (m_listener >= 0) {
            ::close(m_listener);
        }
    }

    tcp_listener(const tcp_listener &) = delete;
    tcp_listener &operator=(const tcp_listener &) = delete;

    /*! \brief Waits for a client to connect and accepts it. */
    bool accept_peer(int timeout_ms = 2000)
    {
        pollfd pfd = {};
        pfd.fd = m_listener;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            return false;
        }

        m_peer = ::accept(m_listener, nullptr, nullptr);
        if (m_peer < 0) {
            return false;
        }

        timeval tv = {};
        tv.tv_sec = 2;
        ::setsockopt(m_peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return true;
    }

    void send(const std::string &payload)
    {
        BOOST_REQUIRE(m_peer >= 0);
        size_t total = 0;
        while (total < payload.size()) {
            ssize_t n = ::send(
                m_peer, payload.data() + total, payload.size() - total, MSG_NOSIGNAL);
            BOOST_REQUIRE(n > 0);
            total += n;
        }
    }

    std::string recv(size_t len)
    {
        BOOST_REQUIRE(m_peer >= 0);
        std::string buf(len, '\0');
        ssize_t n = ::recv(m_peer, buf.data(), len, 0);
        return n < 0 ? std::string() : buf.substr(0, n);
    }

    /*! \brief Drops the accepted connection, keeping the port listening. */
    void close_peer()
    {
        if (m_peer >= 0) {
            ::close(m_peer);
            m_peer = -1;
        }
    }

private:
    int m_listener;
    int m_peer = -1;
};

/*!
 * \brief Builds a DIFI packet of \p size bytes with a valid header.
 *
 * Bits 0 to 15 of the first word hold the size of the packet counted in 32 bit
 * words, which is what a stream transport reads to find the packet boundary.
 * The payload is filled with \p filler so that a test can tell packets apart.
 */
inline std::string difi_packet(size_t size, char filler = 'x')
{
    BOOST_REQUIRE(size >= 4);
    BOOST_REQUIRE(size % 4 == 0);

    std::string packet(size, filler);
    uint32_t header = htonl(static_cast<uint32_t>(size / 4));
    std::memcpy(packet.data(), &header, sizeof(header));
    return packet;
}

/*! \brief Reads a socket level option, for example SO_RCVBUF. */
inline int sock_opt(int fd, int optname)
{
    int value = 0;
    socklen_t len = sizeof(value);
    BOOST_REQUIRE(::getsockopt(fd, SOL_SOCKET, optname, &value, &len) == 0);
    return value;
}

/*! \brief The local port a socket is bound to. */
inline uint16_t local_port(int fd)
{
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, (sockaddr *)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

} // namespace gr::difi::qa

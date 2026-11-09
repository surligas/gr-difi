/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "qa_transport_utils.hpp"
#include <difi/difi.hpp>
#include <difi/difi_sink_cpp.h>
#include <difi/udp_client.hpp>

#include <gnuradio/io_signature.h>
#include <gnuradio/sync_block.h>
#include <gnuradio/top_block.h>

#include <arpa/inet.h>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <complex>
#include <cstring>
#include <string>

using namespace gr::difi;
using namespace gr::difi::qa;
using namespace std::chrono_literals;

namespace {

constexpr double SAMP_RATE = 1e6;
constexpr uint32_t SAMPLES_PER_PACKET = 1000;
constexpr size_t PAYLOAD_BYTES = SAMPLES_PER_PACKET * sizeof(std::complex<int16_t>);

/* The rate at which the emulated digitizer plays out the samples. */
constexpr double DRAIN_BYTES_PER_SEC = SAMP_RATE * sizeof(std::complex<int16_t>);

/* Close to the buffer size that the real digitizer reports. */
constexpr uint64_t BUFFER_SIZE = 400'000'000;

/* 1 MB, a quarter of a second of samples. */
constexpr float MAX_FILL_PCT = 0.25f;
constexpr double TARGET_FILL_BYTES = BUFFER_SIZE * MAX_FILL_PCT / 100.0;

/*!
 * \brief Produces zero samples as fast as the scheduler asks for them.
 *
 * The source must be faster than real time. Otherwise a slow source would limit
 * the data rate and hide a sink that does not pace itself.
 */
class zero_source : public gr::sync_block
{
public:
    zero_source()
        : gr::sync_block("zero_source",
                         gr::io_signature::make(0, 0, 0),
                         gr::io_signature::make(1, 1, sizeof(std::complex<int16_t>)))
    {
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override
    {
        std::memset(output_items[0], 0, noutput_items * sizeof(std::complex<int16_t>));
        return noutput_items;
    }
};

/*!
 * \brief A running flowgraph with a rate-controlled sink that sends to \p port.
 *
 * The flowgraph stops when the object is destroyed.
 */
class paced_sink
{
public:
    explicit paced_sink(uint16_t port) : m_tb(gr::make_top_block("qa_sink_rate_control"))
    {
        auto src = gnuradio::make_block_sptr<zero_source>();
        // The context interval is large, so that the sink sends mostly data
        // packets after the handshake.
        auto snk = difi_sink_cpp<difi::context_version::standard,
                                 udp_client,
                                 difi::bit_depth::bits_16,
                                 difi::byte_order::big>::make(LOOPBACK,
                                                              port,
                                                              SAMP_RATE,
                                                              SAMPLES_PER_PACKET,
                                                              0,
                                                              1000000,
                                                              "",
                                                              10000,
                                                              0.0,
                                                              0.0,
                                                              0.0,
                                                              0.0f,
                                                              0.0f,
                                                              0.0f,
                                                              0.0,
                                                              difi::reference_point::if_converter,
                                                              difi::tsi_type::utc,
                                                              true,
                                                              MAX_FILL_PCT,
                                                              1.0f);
        m_tb->connect(src, 0, snk, 0);
        m_tb->start();
    }

    ~paced_sink()
    {
        m_tb->stop();
        m_tb->wait();
    }

    paced_sink(const paced_sink&) = delete;
    paced_sink& operator=(const paced_sink&) = delete;

private:
    gr::top_block_sptr m_tb;
};

/*!
 * \brief Builds the flow control report of a digitizer.
 *
 * \param level buffer level, from 0 (empty) to 4095 (full)
 * \param nearly_empty value of the nearly empty indicator
 */
std::string report(uint16_t level, bool nearly_empty)
{
    difi::flow_control fc;
    fc.buffer_size = BUFFER_SIZE;
    fc.buffer_level = level;
    fc.nearly_empty = nearly_empty;
    fc.sample_rate = SAMP_RATE;
    auto raw = difi::pack_flow_control(fc);
    return std::string(raw.begin(), raw.end());
}

/*!
 * \brief Counts the payload bytes of the data packets that arrive before
 * \p deadline.
 *
 * The receive buffer of the peer is smaller than a data packet, so the size
 * comes from the packet header and not from the length of the datagram.
 */
size_t data_bytes_until(peer& digitizer, std::chrono::steady_clock::time_point deadline)
{
    size_t total = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string pkt = digitizer.recv();
        if (pkt.size() < sizeof(uint32_t)) {
            continue;
        }
        uint32_t w0 = 0;
        std::memcpy(&w0, pkt.data(), sizeof(w0));
        w0 = ntohl(w0);
        if ((w0 >> 28) == static_cast<uint32_t>(difi::packet_type::data)) {
            total += (w0 & 0xffff) * 4 - difi::DIFI_HEADER_SIZE;
        }
    }
    return total;
}

/*! \brief Seconds elapsed since \p start. */
double seconds_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

/*
 * An idle digitizer reports an empty buffer. The sink may then fill the buffer
 * up to the target, but after that it must send only as fast as the digitizer
 * plays the samples out, even if no new report arrives.
 */
BOOST_AUTO_TEST_CASE(t_empty_report_releases_only_the_target_fill)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    // Until the first report arrives, the sink only sends context packets.
    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));
    const auto start = std::chrono::steady_clock::now();

    size_t sent = data_bytes_until(digitizer, start + 300ms);
    const double elapsed = seconds_since(start);

    // A few packets of margin cover the packet that crosses the target, plus
    // some slack for timing.
    const double allowed =
        TARGET_FILL_BYTES + DRAIN_BYTES_PER_SEC * elapsed + 4 * PAYLOAD_BYTES;
    BOOST_CHECK_GE(sent, TARGET_FILL_BYTES);
    BOOST_CHECK_LE(sent, allowed);
}

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
#include <future>
#include <string>
#include <thread>

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
 * \brief A running flowgraph with a rate-controlled sink.
 *
 * The flowgraph stops when the object is destroyed.
 */
class paced_sink
{
public:
    /*!
     * \brief Builds the flowgraph and starts it.
     *
     * \param port UDP port of the fake digitizer
     * \param max_fill_pct target fill of the digitizer buffer, in percent of
     *        its size
     */
    explicit paced_sink(uint16_t port, float max_fill_pct = MAX_FILL_PCT)
        : m_tb(gr::make_top_block("qa_sink_rate_control"))
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
                                                              max_fill_pct,
                                                              1.0f);
        m_tb->connect(src, 0, snk, 0);
        m_tb->start();
    }

    ~paced_sink() { stop(); }

    /*! \brief Stops the flowgraph and waits for it. Calling it again does nothing. */
    void stop()
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
 * \param nearly_empty true if the report says that the buffer is nearly empty
 * \param underflow true if the report says that the buffer had an underflow
 */
std::string report(uint16_t level, bool nearly_empty, bool underflow = false)
{
    difi::flow_control fc;
    fc.buffer_size = BUFFER_SIZE;
    fc.buffer_level = level;
    fc.nearly_empty = nearly_empty;
    fc.underflow = underflow;
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
 *
 * A receive can block past \p deadline while the sink sends nothing, so a
 * packet that arrives after the deadline does not count.
 */
size_t data_bytes_until(peer& digitizer, std::chrono::steady_clock::time_point deadline)
{
    size_t total = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string pkt = digitizer.recv();
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
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

/*!
 * \brief Stops the flowgraph of \p sink and tells whether it stopped within
 * \p limit.
 *
 * If it takes longer, the fake digitizer sends an empty buffer report. This
 * ends any wait of the sink, so the flowgraph still stops and a failing test
 * does not hang.
 */
bool stops_within(paced_sink& sink, peer& digitizer, std::chrono::milliseconds limit)
{
    auto stopped = std::async(std::launch::async, [&sink] { sink.stop(); });
    if (stopped.wait_for(limit) == std::future_status::ready) {
        return true;
    }
    digitizer.reply(report(0, true));
    stopped.wait();
    return false;
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

/*
 * A digitizer that never answers must not keep the flowgraph from stopping.
 * Until the first report arrives, the sink waits inside work().
 */
BOOST_AUTO_TEST_CASE(t_stops_while_waiting_for_the_first_report)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    // The context packet shows that the sink now waits for a report.
    BOOST_REQUIRE(!digitizer.recv().empty());

    BOOST_CHECK(stops_within(sink, digitizer, 1000ms));
}

/*
 * A report of a full buffer makes the sink wait until the digitizer has
 * transmitted the excess samples, which here takes about 100 s. Stopping the
 * flowgraph must not wait for that.
 */
BOOST_AUTO_TEST_CASE(t_stops_during_a_long_pacing_wait)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(4095, false));
    std::this_thread::sleep_for(200ms);

    BOOST_CHECK(stops_within(sink, digitizer, 1000ms));
}

/*
 * A report can show a fuller buffer than the sink expects, for example when the
 * digitizer transmits slower than the configured sample rate. The sink must
 * then wait until the excess is transmitted, and only then send again.
 */
BOOST_AUTO_TEST_CASE(t_fuller_report_makes_the_sink_wait)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));

    // Let the sink fill the buffer up to the target and start pacing.
    data_bytes_until(digitizer, std::chrono::steady_clock::now() + 300ms);

    // Level 31 is about 3 MB. The excess of about 2 MB above the target takes
    // about 0.5 s to transmit.
    digitizer.reply(report(31, false));
    const auto start = std::chrono::steady_clock::now();

    // Only packets that the sink sent before it received the report may arrive.
    BOOST_CHECK_LE(data_bytes_until(digitizer, start + 300ms), 3 * PAYLOAD_BYTES);
    BOOST_CHECK_GT(data_bytes_until(digitizer, start + 1000ms), 0U);
}

/*
 * A digitizer may report a nearly empty buffer or an underflow while the level
 * is still above the target. The sink must trust the level and wait. It must
 * not send the target fill on top of the samples that are in the buffer.
 */
BOOST_AUTO_TEST_CASE(t_nearly_empty_report_uses_the_level)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    // Level 31 is about 3 MB, which is about 2 MB above the target. The
    // digitizer needs about 0.5 s to transmit this excess.
    digitizer.reply(report(31, true, true));
    const auto start = std::chrono::steady_clock::now();

    // The sink sent no data before this first report, so it must send none
    // until the excess is transmitted.
    BOOST_CHECK_EQUAL(data_bytes_until(digitizer, start + 300ms), 0U);
    BOOST_CHECK_GT(data_bytes_until(digitizer, start + 1000ms), 0U);
}

/*
 * With a target smaller than one packet, there is never room for a packet below
 * the target. The sink must still send close to the rate at which the digitizer
 * transmits.
 */
BOOST_AUTO_TEST_CASE(t_small_target_sends_close_to_real_time)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port, 0.0f);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));
    const auto start = std::chrono::steady_clock::now();

    size_t sent = data_bytes_until(digitizer, start + 500ms);
    const double expected = DRAIN_BYTES_PER_SEC * seconds_since(start);

    // Each sleep ends a little late. With no margin in the buffer, the sink
    // cannot make up for that time, so it sends slightly less than expected.
    BOOST_CHECK_GE(sent, 0.75 * expected);
    BOOST_CHECK_LE(sent, expected + PAYLOAD_BYTES);
}

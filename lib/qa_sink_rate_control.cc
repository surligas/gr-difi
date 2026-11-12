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

/*
 * Buffer levels, from 0 (empty) to 4095 (full). At startup, the sink stops
 * sending above 2 seconds of samples, which is 8 MB or a level of about 82.
 */
constexpr uint16_t LEVEL_1_SECOND = 41;
constexpr uint16_t LEVEL_3_SECONDS = 123;

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
     */
    explicit paced_sink(uint16_t port) : m_tb(gr::make_top_block("qa_sink_rate_control"))
    {
        auto src = gnuradio::make_block_sptr<zero_source>();
        // The context interval is large, so that the sink sends mostly data
        // packets after the handshake.
        m_sink = difi_sink_cpp<difi::context_version::standard,
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
                                                            true);
        m_tb->connect(src, 0, m_sink, 0);
        m_tb->start();
    }

    ~paced_sink() { stop(); }

    /*! \brief Stops the flowgraph and waits for it. Calling it again does nothing. */
    void stop()
    {
        m_tb->stop();
        m_tb->wait();
    }

    /*! \brief The sink block, for calls while the flowgraph runs. */
    difi_sink_base& sink() { return *m_sink; }

    paced_sink(const paced_sink&) = delete;
    paced_sink& operator=(const paced_sink&) = delete;

private:
    gr::top_block_sptr m_tb;
    difi_sink_base::sptr m_sink;
};

/*!
 * \brief Builds the flow control report of a digitizer.
 *
 * \param level buffer level, from 0 (empty) to 4095 (full)
 * \param nearly_empty true if the report says that the buffer is nearly empty
 * \param nearly_full true if the report says that the buffer is nearly full
 * \param underflow true if the report says that the buffer had an underflow
 */
std::string report(uint16_t level,
                   bool nearly_empty,
                   bool nearly_full = false,
                   bool underflow = false)
{
    difi::flow_control fc;
    fc.buffer_size = BUFFER_SIZE;
    fc.buffer_level = level;
    fc.nearly_empty = nearly_empty;
    fc.nearly_full = nearly_full;
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
 * packet that arrives after the deadline does not count. This also means that
 * a call returns only after the receive timeout of the peer (2 s) when the
 * sink is silent.
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

/*! \brief Counts the payload bytes that arrive within \p window from now. */
size_t data_bytes_within(peer& digitizer, std::chrono::milliseconds window)
{
    return data_bytes_until(digitizer, std::chrono::steady_clock::now() + window);
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
 * If it takes longer, the fake digitizer sends a nearly empty report. This
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
 * While the sink fills the buffer, it must not send faster than twice real time.
 * It must not send much slower either, because sleeps that end late must not
 * lower the rate. It must keep sending even if no new report arrives.
 */
BOOST_AUTO_TEST_CASE(t_fills_at_twice_real_time)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    // Until the first report arrives, the sink only sends context packets.
    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));
    const auto start = std::chrono::steady_clock::now();

    // At twice real time, the sink needs about 1 s to send 2 seconds of
    // samples. This window ends before that.
    size_t sent = data_bytes_until(digitizer, start + 300ms);
    const double limit = 2 * DRAIN_BYTES_PER_SEC * seconds_since(start);

    // The margin covers the few packets that the sink sends at once at the
    // start, because its planned send time starts up to 1 ms in the past.
    BOOST_CHECK_LE(sent, limit + 4 * PAYLOAD_BYTES);
    BOOST_CHECK_GE(sent, 0.75 * limit);
}

/*
 * At startup, a report of more than 2 seconds of samples in the buffer stops
 * the sink.
 */
BOOST_AUTO_TEST_CASE(t_report_above_two_seconds_stops_sending)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));
    BOOST_REQUIRE_GT(data_bytes_within(digitizer, 100ms), 0U);

    digitizer.reply(report(LEVEL_3_SECONDS, false));

    // Only packets that the sink sent before it received the report may arrive.
    BOOST_CHECK_LE(data_bytes_within(digitizer, 100ms), 4 * PAYLOAD_BYTES);
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);
}

/*
 * At startup, a nearly full report stops the sink, even below 2 seconds of
 * samples. This covers a buffer that is too small to hold 2 seconds of samples.
 */
BOOST_AUTO_TEST_CASE(t_nearly_full_report_stops_sending)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(0, true));
    BOOST_REQUIRE_GT(data_bytes_within(digitizer, 100ms), 0U);

    digitizer.reply(report(LEVEL_1_SECOND, false, true));

    BOOST_CHECK_LE(data_bytes_within(digitizer, 100ms), 4 * PAYLOAD_BYTES);
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);
}

/*
 * Once stopped, the sink waits for a nearly empty report. A lower level alone
 * must not restart it.
 */
BOOST_AUTO_TEST_CASE(t_only_nearly_empty_restarts_sending)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    // The first report already shows more than 2 seconds of samples, so the
    // sink must send no data at all.
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    digitizer.reply(report(LEVEL_1_SECOND, false));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    digitizer.reply(report(LEVEL_1_SECOND, true));
    BOOST_CHECK_GT(data_bytes_within(digitizer, 300ms), 0U);
}

/*
 * After startup, the sink sends only while the reports say that the buffer is
 * nearly empty. It must stop as soon as the flag clears, even far below 2
 * seconds of samples. This keeps the delay of the samples low.
 */
BOOST_AUTO_TEST_CASE(t_cleared_nearly_empty_stops_sending)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    // End the startup at once.
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    digitizer.reply(report(1, true));
    BOOST_REQUIRE_GT(data_bytes_within(digitizer, 100ms), 0U);

    digitizer.reply(report(LEVEL_1_SECOND, false));

    // Only packets that the sink sent before it received the report may arrive.
    BOOST_CHECK_LE(data_bytes_within(digitizer, 100ms), 4 * PAYLOAD_BYTES);
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);
}

/*
 * After an underflow, the digitizer again waits for 2 seconds of samples. The
 * sink must start the startup again and keep sending until the buffer holds
 * them, even when the nearly empty flag clears on the way.
 */
BOOST_AUTO_TEST_CASE(t_underflow_starts_the_startup_again)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    // End the startup at once.
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    digitizer.reply(report(0, true, false, true));
    BOOST_REQUIRE_GT(data_bytes_within(digitizer, 100ms), 0U);

    // Below 2 seconds, without the nearly empty flag. After the startup this
    // would stop the sink.
    digitizer.reply(report(LEVEL_1_SECOND, false));
    data_bytes_within(digitizer, 50ms);
    BOOST_CHECK_GT(data_bytes_within(digitizer, 200ms), 0U);

    digitizer.reply(report(LEVEL_3_SECONDS, false));
    BOOST_CHECK_LE(data_bytes_within(digitizer, 100ms), 4 * PAYLOAD_BYTES);
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);
}

/*
 * The digitizer may keep the underflow flag set in the reports that follow an
 * underflow. Only the first of them starts the startup again. Otherwise the
 * sink would keep the buffer at 2 seconds of samples.
 */
BOOST_AUTO_TEST_CASE(t_underflow_flag_that_stays_set_starts_the_startup_once)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    digitizer.reply(report(0, true, false, true));
    BOOST_REQUIRE_GT(data_bytes_within(digitizer, 100ms), 0U);

    // The startup ends, while the flag stays set.
    digitizer.reply(report(LEVEL_3_SECONDS, false, false, true));
    BOOST_CHECK_LE(data_bytes_within(digitizer, 100ms), 4 * PAYLOAD_BYTES);
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);

    // The flag is still set, but this is the same underflow, so the sink
    // keeps waiting for a nearly empty report.
    digitizer.reply(report(LEVEL_1_SECOND, false, false, true));
    BOOST_CHECK_EQUAL(data_bytes_within(digitizer, 300ms), 0U);
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
 * The wait for a nearly empty report has no time limit. Stopping the
 * flowgraph must still end it.
 */
BOOST_AUTO_TEST_CASE(t_stops_while_waiting_for_nearly_empty)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    std::this_thread::sleep_for(200ms);

    BOOST_CHECK(stops_within(sink, digitizer, 1000ms));
}

/*
 * Disabling rate control while the sink waits for a nearly empty report must
 * end the wait, so that the sink sends again.
 */
BOOST_AUTO_TEST_CASE(t_disabling_rate_control_ends_the_wait)
{
    uint16_t port = free_port();
    peer digitizer(port);
    paced_sink sink(port);

    BOOST_REQUIRE(!digitizer.recv().empty());
    digitizer.reply(report(LEVEL_3_SECONDS, false));
    std::this_thread::sleep_for(200ms);

    sink.sink().set_rate_control(false);
    BOOST_CHECK_GT(data_bytes_within(digitizer, 300ms), 0U);
}

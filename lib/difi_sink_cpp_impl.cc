// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.

#include "difi_sink_cpp_impl.h"
#include <gnuradio/io_signature.h>
#include <arpa/inet.h>
#include <boost/chrono/duration.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

#include <difi/tcp_client.hpp>
#include <difi/udp_client.hpp>

namespace gr {
namespace difi {

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
difi_sink_base::sptr
difi_sink_cpp<V, Transport, BitDepth, ByteOrder>::make(std::string ip_addr,
                                                       uint16_t port,
                                                       double samp_rate,
                                                       uint32_t samples_per_packet,
                                                       int stream_number,
                                                       int context_interval,
                                                       const std::string& length_tag_name,
                                                       uint32_t lead_time_us,
                                                       double rf_freq,
                                                       double if_freq,
                                                       double bandwidth,
                                                       float rf_gain,
                                                       float if_gain,
                                                       float ref_level,
                                                       double if_band_offset,
                                                       difi::reference_point ref_point,
                                                       difi::tsi_type time_source,
                                                       bool rate_control)
{
    return gnuradio::make_block_sptr<
        difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>>(ip_addr,
                                                               port,
                                                               samp_rate,
                                                               samples_per_packet,
                                                               stream_number,
                                                               context_interval,
                                                               length_tag_name,
                                                               lead_time_us,
                                                               rf_freq,
                                                               if_freq,
                                                               bandwidth,
                                                               rf_gain,
                                                               if_gain,
                                                               ref_level,
                                                               if_band_offset,
                                                               ref_point,
                                                               time_source,
                                                               rate_control);
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::difi_sink_cpp_impl(
    std::string ip_addr,
    uint16_t port,
    double samp_rate,
    uint32_t samples_per_packet,
    int stream_number,
    int context_interval,
    const std::string& length_tag_name,
    uint32_t lead_time_us,
    double rf_freq,
    double if_freq,
    double bandwidth,
    float rf_gain,
    float if_gain,
    float ref_level,
    double if_band_offset,
    difi::reference_point ref_point,
    difi::tsi_type time_source,
    bool rate_control)
    : gr::sync_block("difi_sink_cpp_impl",
                     gr::io_signature::make(1, 1, sizeof(sample_t)),
                     gr::io_signature::make(0, 0, 0)),
      d_stream_number(int(stream_number)),
      d_samp_rate(samp_rate),
      d_pkt_n(0),
      d_samples_since_last_reference(0),
      d_packet_count(0),
      d_context_packet_count(0),
      d_contex_packet_interval(context_interval)
{
    m_rate_control.store(rate_control);
    m_first_fc_received.store(false);
    m_rx_running.store(false);

    this->message_port_register_out(pmt::intern("status"));

    m_transport = std::make_unique<Transport>(ip_addr, port);

    if (!m_transport) {
        throw std::runtime_error("Failed to create transport object");
    }

    if (samples_per_packet < 2) {
        GR_LOG_ERROR(this->d_logger,
                     "samples per packet cannot be less than 2, (to ensure one 32 bit "
                     "word is filled)");
        throw std::runtime_error("samples per packet cannot be less than 2, (to ensure "
                                 "one 32 bit word is filled)");
    }
    m_length_tag_name = length_tag_name;
    m_length_tag_key =
        length_tag_name.empty() ? pmt::PMT_NIL : pmt::intern(length_tag_name);
    d_burst_samples_remaining = 0;
    d_lead_time_us = lead_time_us;
    d_full = 0;
    d_frac = 0;
    m_time_source = time_source;
    d_static_bits = difi::make_data_static_bits(m_time_source);
    d_samples_per_packet = samples_per_packet;
    d_data_len = samples_per_packet * s_bytes_per_sample;

    m_context = difi::make_context<V>({
        .sample_rate = static_cast<double>(samp_rate),
        .depth = s_bit_depth_val,
        .stream_id = static_cast<uint32_t>(d_stream_number),
        .rf_freq = rf_freq,
        .if_freq = if_freq,
        .bandwidth = bandwidth,
        .if_band_offset = if_band_offset,
        .rf_gain = rf_gain,
        .if_gain = if_gain,
        .ref_level = ref_level,
        .full = 0,
        .frac = 0,
        .ref_point = ref_point,
        .tsi = time_source,
    });
    m_context_buffer.resize(m_context.packet_size);

    update_class_id(rate_control);
    m_data_buffer.resize(difi::DIFI_HEADER_SIZE + d_data_len);
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::update_class_id(
    bool rate_control_enabled)
{
    const auto info_cls = rate_control_enabled
                              ? difi::information_class::bit_padding_data_plane
                              : difi::information_class::basic_data_plane;
    constexpr auto id_oui =
        (V == difi::context_version::alternative) ? difi::oui::vita : difi::oui::difi;

    m_context.class_id =
        difi::make_class_id(id_oui, difi::packet_class::standard_context, info_cls);
    m_class_id =
        difi::make_class_id(id_oui, difi::packet_class::standard_data, info_cls);
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::~difi_sink_cpp_impl()
{
    stop_rx_thread();
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
bool difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::start()
{
    if (m_rate_control.load()) {
        {
            gr::thread::scoped_lock lock(m_fc_mutex);
            m_filling = true;
            m_started = false;
            m_underflow = false;
        }
        m_first_fc_received.store(false);
        start_rx_thread();
    }
    return gr::sync_block::start();
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
bool difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::stop()
{
    stop_rx_thread();
    return gr::sync_block::stop();
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::start_rx_thread()
{
    if (!m_rx_running.exchange(true)) {
        m_rx_thread = std::thread(&difi_sink_cpp_impl::rx_loop, this);
    }
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::stop_rx_thread()
{
    if (m_rx_running.exchange(false)) {
        wake_rate_control();
        if (m_rx_thread.joinable()) {
            m_rx_thread.join();
        }
    }
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::rx_loop()
{
    uint8_t rx_buf[1500];
    while (m_rx_running.load()) {
        ssize_t ret = m_transport->recv(rx_buf, sizeof(rx_buf));
        if (ret < static_cast<ssize_t>(difi::HEADER_SIZE)) {
            continue;
        }

        std::span<const uint8_t> pkt(rx_buf, static_cast<size_t>(ret));
        auto h = difi::parse_header(pkt);

        switch (h.type) {
        case difi::packet_type::command: {
            auto fc_opt = difi::parse_flow_control(pkt);
            if (fc_opt) {
                {
                    gr::thread::scoped_lock lock(m_fc_mutex);
                    m_last_fc = *fc_opt;
                    update_fill_state(*fc_opt);
                    m_first_fc_received.store(true);
                }
                m_fc_cv.notify_all();
                this->message_port_pub(pmt::intern("status"),
                                       difi::flow_control_to_pmt(*fc_opt));
            }
            break;
        }
        case difi::packet_type::context: {
            try {
                auto ctx_var = difi::parse_context(pkt);
                this->message_port_pub(pmt::intern("status"),
                                       difi::context_to_pmt(h, ctx_var));
            } catch (const std::exception& e) {
                GR_LOG_WARN(this->d_logger,
                            "Failed to parse received context packet: " +
                                std::string(e.what()));
            }
            break;
        }
        default:
            break;
        }
    }
}

/**
 * @brief Returns the rate at which the digitizer plays out samples, in bytes
 * per second.
 *
 * It uses the configured sample rate, or the one of the last report if none
 * was configured. The caller must hold m_fc_mutex.
 *
 * @return the rate in bytes per second, or 0 if no sample rate is known
 */
template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
double difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::drain_rate_bytes() const
{
    const double samp_rate = (d_samp_rate > 0.0) ? d_samp_rate : m_last_fc.sample_rate;
    return std::max(0.0, samp_rate) * s_bytes_per_sample;
}

/**
 * @brief Decides from a flow control report whether the sink sends or waits.
 *
 * At startup, the sink sends until the report shows more than s_fill_seconds
 * of samples in the buffer, because the digitizer transmits nothing before
 * that. It also stops when the digitizer reports a nearly full buffer. This is
 * needed when the buffer is smaller than s_fill_seconds of samples.
 *
 * After that, the sink sends only while the digitizer says that the buffer is
 * nearly empty. This keeps the buffer close to the nearly empty level, so
 * that the samples wait as little time as possible in the buffer. After
 * startup, the sink ignores the level and looks only at the flag.
 *
 * After an underflow, the digitizer again waits for s_fill_seconds of samples
 * before it transmits, so the startup begins again. Only the first report
 * with the underflow flag set does this. The digitizer may keep the flag set
 * in the next reports, and starting again on each of them would keep the
 * buffer at s_fill_seconds of samples.
 *
 * The caller must hold m_fc_mutex.
 *
 * @param fc the report that has just arrived
 */
template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::update_fill_state(
    const difi::flow_control& fc)
{
    const bool new_underflow = fc.underflow && !m_underflow;
    m_underflow = fc.underflow;
    if (new_underflow) {
        m_started = false;
        m_filling = true;
    }

    if (m_started) {
        m_filling = fc.nearly_empty;
        return;
    }

    const double level_bytes = static_cast<double>(fc.buffer_size) *
                               (static_cast<double>(fc.buffer_level) / 4095.0);
    const double target_bytes = s_fill_seconds * drain_rate_bytes();
    if (fc.nearly_full || level_bytes > target_bytes) {
        m_started = true;
        m_filling = false;
    }
}

/**
 * @brief Wakes work() if it waits in wait_rate_control().
 *
 * Call it after a change of m_rx_running or m_rate_control. With the mutex
 * taken, work() either sees the change before it waits, or is already waiting
 * when the notification comes. Without the mutex, the notification could
 * arrive between the check and the wait, and work() would wait for a report
 * that never comes.
 */
template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::wake_rate_control()
{
    {
        gr::thread::scoped_lock lock(m_fc_mutex);
    }
    m_fc_cv.notify_all();
}

/**
 * @brief Delays the next packet as rate control requires.
 *
 * The sink first waits for the first flow control report. After that it sends
 * at twice real time while update_fill_state() allows it, and sends nothing
 * otherwise.
 *
 * It returns at once when rate control is disabled, and early when the
 * receive thread stops.
 *
 * @param num_samples number of samples in the next packet
 */
template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::wait_rate_control(
    uint32_t num_samples)
{
    if (!m_rate_control.load()) {
        return;
    }

    if (!m_rx_running.load()) {
        start_rx_thread();
    }

    // Handshake: wait for the first flow control frame
    if (!m_first_fc_received.load()) {
        gr::thread::scoped_lock lock(m_fc_mutex);
        while (m_rx_running.load() && !m_first_fc_received.load()) {
            lock.unlock();
            send_context();
            lock.lock();
            m_fc_cv.wait_for(lock, boost::chrono::milliseconds(500), [this]() {
                return !m_rx_running.load() || m_first_fc_received.load();
            });
        }
    }

    if (!m_rx_running.load()) {
        return;
    }

    gr::thread::scoped_lock lock(m_fc_mutex);
    const auto stopped = [this]() {
        return !m_rx_running.load() || !m_rate_control.load();
    };

    /*
     * Wait while the digitizer plays out the samples in its buffer. This can
     * take seconds, so the wait has no timeout. Only a report or a stop ends
     * it.
     */
    m_fc_cv.wait(lock, [this, &stopped]() { return stopped() || m_filling; });
    if (stopped()) {
        return;
    }

    const double drain_rate = drain_rate_bytes();
    if (drain_rate <= 0.0) {
        return;
    }

    /*
     * Send at no more than twice real time.
     *
     * Move the planned send time forward by a fixed step for each packet,
     * not from the current time. A sleep that ends late is then made up
     * by the next packets, so the average rate does not drop.
     * After a long pause, for example a wait for the buffer to drain,
     * start again close to the current time, so that the sink does not
     * send all the missed packets at once.
     */
    const auto now = std::chrono::steady_clock::now();
    m_next_send_time = std::max(m_next_send_time, now - s_max_send_lag);
    if (m_next_send_time > now) {
        const auto sleep_ns = std::chrono::nanoseconds(m_next_send_time - now).count();
        m_fc_cv.wait_for(lock, boost::chrono::nanoseconds(sleep_ns), stopped);
        if (stopped()) {
            return;
        }
    }
    const double pkt_bytes = static_cast<double>(num_samples) * s_bytes_per_sample;
    m_next_send_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(pkt_bytes / (s_max_rate_factor * drain_rate)));
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::reanchor_timestamp()
{
    auto [full, frac] = difi::system_timestamp(d_lead_time_us, m_time_source);
    d_full = full;
    d_frac = frac;
    d_samples_since_last_reference = 0;
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
int difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::work(
    int noutput_items,
    gr_vector_const_void_star& input_items,
    gr_vector_void_star& output_items)
{
    const sample_t* in = reinterpret_cast<const sample_t*>(input_items[0]);
    auto abs_offset = this->nitems_read(0);

    // Stream mode (continuous streaming)
    if (m_length_tag_name.empty()) {
        int n_packets = noutput_items / d_samples_per_packet;
        for (int p = 0; p < n_packets; p++) {
            wait_rate_control(d_samples_per_packet);
            if (!m_rx_running.load() && m_rate_control.load()) {
                return -1;
            }
            if (d_packet_count % d_contex_packet_interval == 0) {
                send_context();
            }
            difi::pack_samples<ByteOrder>(in + p * d_samples_per_packet,
                                          d_samples_per_packet,
                                          m_data_buffer.data() + difi::DIFI_HEADER_SIZE,
                                          s_bit_depth_val);
            auto to_send = pack_data(d_data_len);
            m_transport->send(to_send.data(), to_send.size());
            d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
            d_samples_since_last_reference += d_samples_per_packet;
            d_packet_count++;
        }
        return n_packets * d_samples_per_packet;
    }

    // Burst mode (tagged stream)
    std::vector<tag_t> tags;
    this->get_tags_in_range(tags, 0, abs_offset, abs_offset + noutput_items);
    std::sort(tags.begin(), tags.end(), [](const tag_t& a, const tag_t& b) {
        return a.offset < b.offset;
    });

    if (d_burst_samples_remaining == 0) {
        std::optional<tag_t> next_tag;
        for (const auto& tag : tags) {
            if (pmt::eqv(m_length_tag_key, tag.key) && tag.offset >= abs_offset) {
                next_tag = tag;
                break;
            }
        }

        if (!next_tag) {
            // No burst in current buffer; drop all idle samples
            return noutput_items;
        }

        if (next_tag->offset > abs_offset) {
            // Drop idle samples up to the start of the burst
            return static_cast<int>(next_tag->offset - abs_offset);
        }

        // Burst starts at abs_offset
        d_burst_samples_remaining = pmt::to_uint64(next_tag->value);
        GR_LOG_DEBUG(this->d_logger,
                     "Starting burst at offset " + std::to_string(next_tag->offset) +
                         " with " + std::to_string(d_burst_samples_remaining) +
                         " samples remaining");
        reanchor_timestamp();
    } else {
        // Check for overlapping burst tags
        for (const auto& tag : tags) {
            if (pmt::eqv(m_length_tag_key, tag.key)) {
                if (tag.offset == abs_offset) {
                    this->d_logger->warn(
                        "Overlapping bursts detected. Previous burst will be truncated.");
                    d_burst_samples_remaining = pmt::to_uint64(tag.value);
                    reanchor_timestamp();
                    break;
                } else if (tag.offset > abs_offset &&
                           tag.offset < abs_offset + d_burst_samples_remaining) {
                    this->d_logger->warn(
                        "Overlapping bursts detected. Previous burst will be truncated.");
                    d_burst_samples_remaining = tag.offset - abs_offset;
                    break;
                }
            }
        }
    }

    uint64_t available_for_burst =
        std::min(static_cast<uint64_t>(noutput_items), d_burst_samples_remaining);
    int n_packets = available_for_burst / d_samples_per_packet;
    int samples_consumed = 0;

    for (int p = 0; p < n_packets; p++) {
        wait_rate_control(d_samples_per_packet);
        if (!m_rx_running.load() && m_rate_control.load()) {
            return -1;
        }
        if (d_packet_count % d_contex_packet_interval == 0) {
            send_context();
        }
        difi::pack_samples<ByteOrder>(in + p * d_samples_per_packet,
                                      d_samples_per_packet,
                                      m_data_buffer.data() + difi::DIFI_HEADER_SIZE,
                                      s_bit_depth_val);
        auto to_send = pack_data(d_data_len);
        m_transport->send(to_send.data(), to_send.size());
        d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
        d_samples_since_last_reference += d_samples_per_packet;
        d_packet_count++;
    }

    samples_consumed = n_packets * d_samples_per_packet;
    d_burst_samples_remaining -= samples_consumed;

    // At the end of the burst, send any remaining samples immediately
    if (d_burst_samples_remaining > 0 &&
        d_burst_samples_remaining < d_samples_per_packet) {
        if (static_cast<uint64_t>(noutput_items - samples_consumed) >=
            d_burst_samples_remaining) {
            uint32_t tail_samples = static_cast<uint32_t>(d_burst_samples_remaining);
            size_t tail_payload_bytes = tail_samples * s_bytes_per_sample;
            wait_rate_control(tail_samples);
            if (!m_rx_running.load() && m_rate_control.load()) {
                return -1;
            }
            if (d_packet_count % d_contex_packet_interval == 0) {
                send_context();
            }
            difi::pack_samples<ByteOrder>(in + samples_consumed,
                                          tail_samples,
                                          m_data_buffer.data() + difi::DIFI_HEADER_SIZE,
                                          s_bit_depth_val);
            auto to_send = pack_data(tail_payload_bytes);
            GR_LOG_DEBUG(this->d_logger,
                         "Sending final burst packet with " +
                             std::to_string(tail_samples) + " samples (" +
                             std::to_string(tail_payload_bytes) + " bytes)");
            m_transport->send(to_send.data(), to_send.size());
            d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
            d_samples_since_last_reference += tail_samples;
            d_packet_count++;
            samples_consumed += tail_samples;
            d_burst_samples_remaining = 0;
        }
    }
    GR_LOG_DEBUG(this->d_logger,
                 "Consumed " + std::to_string(samples_consumed) + " samples, " +
                     std::to_string(d_burst_samples_remaining) + " remaining in burst, " +
                     std::to_string(available_for_burst) + " available");
    return samples_consumed;
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
std::span<const uint8_t>
difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::pack_data(size_t payload_bytes)
{
    // VITA 49 requires packet sizes to be a multiple of 32-bit (4-byte) words.
    size_t aligned_payload_bytes = (payload_bytes + 3) & ~size_t(3);
    if (aligned_payload_bytes > payload_bytes) {
        std::memset(&m_data_buffer[difi::DIFI_HEADER_SIZE + payload_bytes],
                    0,
                    aligned_payload_bytes - payload_bytes);
    }

    auto [full, frac] = add_frac_full();
    uint16_t packet_size_words =
        static_cast<uint16_t>((aligned_payload_bytes + difi::DIFI_HEADER_SIZE) / 4);
    uint32_t header =
        d_static_bits ^ (static_cast<uint32_t>(d_pkt_n) << 16) ^ packet_size_words;
    difi::pack_u32(&m_data_buffer[0], header);
    difi::pack_u32(&m_data_buffer[4], d_stream_number);
    difi::pack_u64(&m_data_buffer[8], m_class_id);
    difi::pack_u32(&m_data_buffer[16], full);
    difi::pack_u64(&m_data_buffer[20], frac);
    return std::span<const uint8_t>(m_data_buffer.data(),
                                    difi::DIFI_HEADER_SIZE + aligned_payload_bytes);
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::send_context()
{
    if constexpr (V == difi::context_version::standard) {
        auto [full, frac] = add_frac_full();
        m_context.full = full;
        m_context.frac = frac;
        m_context.tsi = m_time_source;
    }
    difi::pack_context(m_context,
                       d_context_packet_count,
                       d_stream_number,
                       std::span<uint8_t>(m_context_buffer));
    m_transport->send(m_context_buffer.data(), m_context_buffer.size());
    d_context_packet_count = (d_context_packet_count + 1) % difi::VITA_PKT_MOD;
}

template <difi::context_version V,
          sink_transport Transport,
          difi::bit_depth BitDepth,
          difi::byte_order ByteOrder>
requires valid_bit_depth<BitDepth>
std::pair<uint32_t, uint64_t>
difi_sink_cpp_impl<V, Transport, BitDepth, ByteOrder>::add_frac_full()
{
    return difi::advance_timestamp(
        d_full, d_frac, d_samples_since_last_reference, d_samp_rate);
}

/*
 * Instantiates the block and its implementation for one context version,
 * transport and bit depth, once for each byte order. byte_order::network is
 * the same value as byte_order::big, so it needs no instantiation of its own.
 */
#define DIFI_SINK_INSTANTIATE(V, TRANSPORT, DEPTH)                                 \
    template class difi_sink_cpp<V, TRANSPORT, DEPTH, difi::byte_order::big>;      \
    template class difi_sink_cpp<V, TRANSPORT, DEPTH, difi::byte_order::little>;   \
    template class difi_sink_cpp_impl<V, TRANSPORT, DEPTH, difi::byte_order::big>; \
    template class difi_sink_cpp_impl<V, TRANSPORT, DEPTH, difi::byte_order::little>;

DIFI_SINK_INSTANTIATE(difi::context_version::standard,
                      udp_client,
                      difi::bit_depth::bits_16)
DIFI_SINK_INSTANTIATE(difi::context_version::standard,
                      udp_client,
                      difi::bit_depth::bits_8)
DIFI_SINK_INSTANTIATE(difi::context_version::standard,
                      tcp_client,
                      difi::bit_depth::bits_16)
DIFI_SINK_INSTANTIATE(difi::context_version::standard,
                      tcp_client,
                      difi::bit_depth::bits_8)
DIFI_SINK_INSTANTIATE(difi::context_version::alternative,
                      udp_client,
                      difi::bit_depth::bits_16)
DIFI_SINK_INSTANTIATE(difi::context_version::alternative,
                      udp_client,
                      difi::bit_depth::bits_8)
DIFI_SINK_INSTANTIATE(difi::context_version::alternative,
                      tcp_client,
                      difi::bit_depth::bits_16)
DIFI_SINK_INSTANTIATE(difi::context_version::alternative,
                      tcp_client,
                      difi::bit_depth::bits_8)

#undef DIFI_SINK_INSTANTIATE

} /* namespace difi */
} /* namespace gr */

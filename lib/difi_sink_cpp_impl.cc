// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.

#include "difi_sink_cpp_impl.h"
#include <gnuradio/io_signature.h>
#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

#include <difi/tcp_client.hpp>
#include <difi/udp_client.hpp>

namespace gr {
namespace difi {

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
difi_sink_base::sptr
difi_sink_cpp<V, Transport, BitDepth>::make(std::string ip_addr,
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
                                            bool rate_control,
                                            float max_buffer_fill_pct,
                                            float buffer_fill_delta)
{
    return gnuradio::make_block_sptr<difi_sink_cpp_impl<V, Transport, BitDepth>>(
        ip_addr,
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
        rate_control,
        max_buffer_fill_pct,
        buffer_fill_delta);
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
difi_sink_cpp_impl<V, Transport, BitDepth>::difi_sink_cpp_impl(
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
    bool rate_control,
    float max_buffer_fill_pct,
    float buffer_fill_delta)
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
    m_max_buffer_fill_pct.store(max_buffer_fill_pct);
    m_buffer_fill_delta.store(buffer_fill_delta);
    m_first_fc_received.store(false);
    m_rx_running.store(false);
    m_fc_published = false;

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

    m_class_id = (m_context.class_id & ~0xffffULL) |
                 static_cast<uint16_t>(difi::packet_class::standard_data);
    m_data_buffer.resize(difi::DIFI_HEADER_SIZE + d_data_len);
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
difi_sink_cpp_impl<V, Transport, BitDepth>::~difi_sink_cpp_impl()
{
    stop_rx_thread();
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
bool difi_sink_cpp_impl<V, Transport, BitDepth>::start()
{
    if (m_rate_control.load()) {
        {
            std::lock_guard<std::mutex> lock(m_fc_mutex);
            m_est_fill_bytes = 0.0;
            m_last_update_time = std::chrono::steady_clock::now();
        }
        m_first_fc_received.store(false);
        m_fc_published = false;
        start_rx_thread();
    }
    return gr::sync_block::start();
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
bool difi_sink_cpp_impl<V, Transport, BitDepth>::stop()
{
    stop_rx_thread();
    return gr::sync_block::stop();
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::start_rx_thread()
{
    if (!m_rx_running.exchange(true)) {
        m_rx_thread = std::thread(&difi_sink_cpp_impl::rx_loop, this);
    }
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::stop_rx_thread()
{
    if (m_rx_running.exchange(false)) {
        m_fc_cv.notify_all();
        if (m_rx_thread.joinable()) {
            m_rx_thread.join();
        }
    }
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::rx_loop()
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
                bool should_wake = false;
                {
                    std::lock_guard<std::mutex> lock(m_fc_mutex);
                    bool first = !m_first_fc_received.load();
                    m_last_fc = *fc_opt;
                    m_last_update_time = std::chrono::steady_clock::now();

                    if (fc_opt->underflow || fc_opt->nearly_empty ||
                        fc_opt->buffer_level == 0) {
                        m_est_fill_bytes = 0.0;
                        should_wake = true;
                    } else if (fc_opt->buffer_size > 0) {
                        m_est_fill_bytes =
                            static_cast<double>(fc_opt->buffer_size) *
                            (static_cast<double>(fc_opt->buffer_level) / 4095.0);
                        if (fc_opt->buffer_fill_pct() < m_max_buffer_fill_pct.load()) {
                            should_wake = true;
                        }
                    }
                    m_first_fc_received.store(true);
                    if (first) {
                        should_wake = true;
                    }
                }
                if (should_wake) {
                    m_fc_cv.notify_all();
                }
                check_and_publish_flow_control(*fc_opt);
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

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::check_and_publish_flow_control(
    const difi::flow_control& fc)
{
    bool should_publish = false;
    float delta = m_buffer_fill_delta.load();
    float pct = fc.buffer_fill_pct();

    if (delta <= 0.0f) {
        should_publish = true;
    } else if (!m_fc_published) {
        should_publish = true;
    } else if (fc.overflow != m_last_published_overflow ||
               fc.nearly_full != m_last_published_nearly_full ||
               fc.nearly_empty != m_last_published_nearly_empty ||
               fc.underflow != m_last_published_underflow) {
        should_publish = true;
    } else if (std::abs(pct - m_last_published_pct) >= delta) {
        should_publish = true;
    }

    if (should_publish) {
        m_fc_published = true;
        m_last_published_pct = pct;
        m_last_published_overflow = fc.overflow;
        m_last_published_nearly_full = fc.nearly_full;
        m_last_published_nearly_empty = fc.nearly_empty;
        m_last_published_underflow = fc.underflow;

        pmt::pmt_t msg = difi::flow_control_to_pmt(fc);
        this->message_port_pub(pmt::intern("status"), msg);
    }
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::update_estimated_fill(
    std::chrono::steady_clock::time_point now)
{
    if (m_last_update_time.time_since_epoch().count() > 0) {
        double elapsed_s =
            std::chrono::duration<double>(now - m_last_update_time).count();
        if (elapsed_s > 0.0) {
            double samp_rate =
                (d_samp_rate > 0.0) ? d_samp_rate : m_last_fc.sample_rate;
            if (samp_rate > 0.0) {
                double drained_bytes = elapsed_s * samp_rate * s_bytes_per_sample;
                m_est_fill_bytes = std::max(0.0, m_est_fill_bytes - drained_bytes);
            }
        }
    }
    m_last_update_time = now;
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::wait_rate_control(uint32_t num_samples)
{
    if (!m_rate_control.load()) {
        return;
    }

    if (!m_rx_running.load()) {
        start_rx_thread();
    }

    // Handshake: wait for the first flow control frame
    if (!m_first_fc_received.load()) {
        std::unique_lock<std::mutex> lock(m_fc_mutex);
        while (m_rx_running.load() && !m_first_fc_received.load()) {
            lock.unlock();
            send_context();
            lock.lock();
            m_fc_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return !m_rx_running.load() || m_first_fc_received.load();
            });
        }
    }

    if (!m_rx_running.load()) {
        return;
    }

    // Rate limiting pacing:
    // 1. Push samples until estimated or reported fill reaches max_buffer_fill_pct.
    // 2. Once reached, sleep for the time needed to drain the excess above threshold.
    // 3. Wake immediately if a flow control packet reports nearly_empty, underflow, empty,
    //    or fill below max_buffer_fill_pct.
    std::unique_lock<std::mutex> lock(m_fc_mutex);
    update_estimated_fill(std::chrono::steady_clock::now());

    double samp_rate = (d_samp_rate > 0.0) ? d_samp_rate : m_last_fc.sample_rate;
    double drain_rate_Bps = samp_rate * s_bytes_per_sample;
    double pkt_bytes = static_cast<double>(num_samples) * s_bytes_per_sample;

    if (m_last_fc.buffer_size > 0 && drain_rate_Bps > 0.0) {
        double max_pct =
            std::clamp(static_cast<double>(m_max_buffer_fill_pct.load()), 0.0, 100.0);
        double max_fill_bytes =
            static_cast<double>(m_last_fc.buffer_size) * (max_pct / 100.0);

        if (m_est_fill_bytes >= max_fill_bytes) {
            double bytes_to_drain = (m_est_fill_bytes - max_fill_bytes) + pkt_bytes;
            uint64_t sleep_ns = static_cast<uint64_t>(
                (bytes_to_drain / drain_rate_Bps) * 1000000000.0);
            if (sleep_ns > 0) {
                m_fc_cv.wait_for(
                    lock, std::chrono::nanoseconds(sleep_ns), [this, max_fill_bytes]() {
                        return !m_rx_running.load() || !m_rate_control.load() ||
                               m_last_fc.nearly_empty || m_last_fc.underflow ||
                               m_last_fc.buffer_level == 0 ||
                               m_est_fill_bytes < max_fill_bytes;
                    });
                update_estimated_fill(std::chrono::steady_clock::now());
                if (m_est_fill_bytes > max_fill_bytes - pkt_bytes) {
                    m_est_fill_bytes = std::max(0.0, max_fill_bytes - pkt_bytes);
                }
            }
        }

        m_est_fill_bytes += pkt_bytes;
    }
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::reanchor_timestamp()
{
    auto [full, frac] = difi::system_timestamp(d_lead_time_us, m_time_source);
    d_full = full;
    d_frac = frac;
    d_samples_since_last_reference = 0;
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
int difi_sink_cpp_impl<V, Transport, BitDepth>::work(
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
            difi::pack_samples(in + p * d_samples_per_packet,
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
        difi::pack_samples(in + p * d_samples_per_packet,
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
            difi::pack_samples(in + samples_consumed,
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

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
std::span<const uint8_t>
difi_sink_cpp_impl<V, Transport, BitDepth>::pack_data(size_t payload_bytes)
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

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::send_context()
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

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
std::pair<uint32_t, uint64_t> difi_sink_cpp_impl<V, Transport, BitDepth>::add_frac_full()
{
    return difi::advance_timestamp(
        d_full, d_frac, d_samples_since_last_reference, d_samp_rate);
}

template class difi_sink_cpp<difi::context_version::standard,
                             udp_client,
                             difi::bit_depth::bits_16>;
template class difi_sink_cpp<difi::context_version::standard,
                             udp_client,
                             difi::bit_depth::bits_8>;
template class difi_sink_cpp<difi::context_version::standard,
                             tcp_client,
                             difi::bit_depth::bits_16>;
template class difi_sink_cpp<difi::context_version::standard,
                             tcp_client,
                             difi::bit_depth::bits_8>;

template class difi_sink_cpp<difi::context_version::alternative,
                             udp_client,
                             difi::bit_depth::bits_16>;
template class difi_sink_cpp<difi::context_version::alternative,
                             udp_client,
                             difi::bit_depth::bits_8>;
template class difi_sink_cpp<difi::context_version::alternative,
                             tcp_client,
                             difi::bit_depth::bits_16>;
template class difi_sink_cpp<difi::context_version::alternative,
                             tcp_client,
                             difi::bit_depth::bits_8>;

template class difi_sink_cpp_impl<difi::context_version::standard,
                                  udp_client,
                                  difi::bit_depth::bits_16>;
template class difi_sink_cpp_impl<difi::context_version::standard,
                                  udp_client,
                                  difi::bit_depth::bits_8>;
template class difi_sink_cpp_impl<difi::context_version::standard,
                                  tcp_client,
                                  difi::bit_depth::bits_16>;
template class difi_sink_cpp_impl<difi::context_version::standard,
                                  tcp_client,
                                  difi::bit_depth::bits_8>;

template class difi_sink_cpp_impl<difi::context_version::alternative,
                                  udp_client,
                                  difi::bit_depth::bits_16>;
template class difi_sink_cpp_impl<difi::context_version::alternative,
                                  udp_client,
                                  difi::bit_depth::bits_8>;
template class difi_sink_cpp_impl<difi::context_version::alternative,
                                  tcp_client,
                                  difi::bit_depth::bits_16>;
template class difi_sink_cpp_impl<difi::context_version::alternative,
                                  tcp_client,
                                  difi::bit_depth::bits_8>;

} /* namespace difi */
} /* namespace gr */

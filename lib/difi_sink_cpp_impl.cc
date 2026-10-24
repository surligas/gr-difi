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
                                            uint64_t samp_rate,
                                            uint32_t samples_per_packet,
                                            int stream_number,
                                            int context_interval,
                                            uint32_t reference_time_full,
                                            uint64_t reference_time_frac,
                                            const std::string& length_tag_name,
                                            uint32_t lead_time_us,
                                            double rf_freq,
                                            double if_freq,
                                            double bandwidth,
                                            float rf_gain,
                                            float if_gain,
                                            float ref_level,
                                            double if_band_offset,
                                            difi::reference_point ref_point)
{
    return gnuradio::make_block_sptr<difi_sink_cpp_impl<V, Transport, BitDepth>>(
        ip_addr,
        port,
        samp_rate,
        samples_per_packet,
        stream_number,
        context_interval,
        reference_time_full,
        reference_time_frac,
        length_tag_name,
        lead_time_us,
        rf_freq,
        if_freq,
        bandwidth,
        rf_gain,
        if_gain,
        ref_level,
        if_band_offset,
        ref_point);
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
difi_sink_cpp_impl<V, Transport, BitDepth>::difi_sink_cpp_impl(
    std::string ip_addr,
    uint16_t port,
    uint64_t samp_rate,
    uint32_t samples_per_packet,
    int stream_number,
    int context_interval,
    uint32_t reference_time_full,
    uint64_t reference_time_frac,
    const std::string& length_tag_name,
    uint32_t lead_time_us,
    double rf_freq,
    double if_freq,
    double bandwidth,
    float rf_gain,
    float if_gain,
    float ref_level,
    double if_band_offset,
    difi::reference_point ref_point)
    : gr::sync_block("difi_sink_cpp_impl",
                     gr::io_signature::make(1, 1, sizeof(sample_t)),
                     gr::io_signature::make(0, 0, 0)),
      d_stream_number(int(stream_number)),
      d_full_samp(samp_rate),
      d_pkt_n(0),
      d_samples_since_last_reference(0),
      d_packet_count(0),
      d_context_packet_count(0),
      d_contex_packet_interval(context_interval)
{
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
    d_full = reference_time_full;
    d_frac = reference_time_frac;
    d_static_bits = difi::STANDARD_DATA_STATIC_BITS;
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
        .full = reference_time_full,
        .frac = reference_time_frac,
        .ref_point = ref_point,
    });
    m_context_buffer.resize(m_context.packet_size);

    m_class_id = (m_context.class_id & ~0xffffULL) |
                 static_cast<uint16_t>(difi::packet_class::standard_data);
    m_data_buffer.resize(difi::DIFI_HEADER_SIZE + d_data_len);
}

template <difi::context_version V, sink_transport Transport, difi::bit_depth BitDepth>
requires valid_bit_depth<BitDepth>
void difi_sink_cpp_impl<V, Transport, BitDepth>::reanchor_timestamp()
{
    auto [full, frac] = difi::system_timestamp(d_lead_time_us);
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
        this->d_logger->debug("Starting burst at offset {} with {} samples remaining",
                              next_tag->offset,
                              d_burst_samples_remaining);
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
            if (d_packet_count % d_contex_packet_interval == 0) {
                send_context();
            }
            difi::pack_samples(in + samples_consumed,
                               tail_samples,
                               m_data_buffer.data() + difi::DIFI_HEADER_SIZE,
                               s_bit_depth_val);
            auto to_send = pack_data(tail_payload_bytes);
            this->d_logger->debug("Sending final burst packet with {} samples ({} bytes)",
                                  tail_samples,
                                  tail_payload_bytes);
            m_transport->send(to_send.data(), to_send.size());
            d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
            d_samples_since_last_reference += tail_samples;
            d_packet_count++;
            samples_consumed += tail_samples;
            d_burst_samples_remaining = 0;
        }
    }
    this->d_logger->debug("Consumed {} samples, {} remaining in burst, {} available",
                          samples_consumed,
                          d_burst_samples_remaining,
                          available_for_burst);
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
        d_full, d_frac, d_samples_since_last_reference, d_full_samp);
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

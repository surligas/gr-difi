// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.

#include <gnuradio/io_signature.h>
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <memory>
#include "difi_sink_cpp_impl.h"

#include <difi/tcp_client.hpp>
#include <difi/udp_client.hpp>

namespace gr {
  namespace difi {

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    difi_sink_base::sptr
    difi_sink_cpp<V, Transport, BitDepth>::make(
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        uint32_t reference_time_full,
        uint64_t reference_time_frac,
        bool burst_mode,
        uint32_t lead_time_us)
    {
      return gnuradio::make_block_sptr<difi_sink_cpp_impl<V, Transport, BitDepth>>(
          ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
          reference_time_full, reference_time_frac, burst_mode, lead_time_us);
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
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
        bool burst_mode,
        uint32_t lead_time_us)
      : gr::sync_block("difi_sink_cpp_impl",
              gr::io_signature::make(1, 1, sizeof(sample_t)),
              gr::io_signature::make(0, 0, 0)),
              d_stream_number(int(stream_number)),
              d_full_samp(samp_rate),
              d_pkt_n(0),
              d_current_buff_idx(0),
              d_pcks_since_last_reference(0),
              d_packet_count(0),
              d_context_packet_count(0),
              d_contex_packet_interval(context_interval)
    {
      m_transport = std::make_unique<Transport>(ip_addr, port);

      if (samples_per_packet < 2)
      {
        GR_LOG_ERROR(this->d_logger, "samples per packet cannot be less than 2, (to ensure one 32 bit word is filled)");
        throw std::runtime_error("samples per packet cannot be less than 2, (to ensure one 32 bit word is filled)");
      }
      d_tx_sob_key = pmt::intern("tx_sob");
      d_tx_eob_key = pmt::intern("tx_eob");
      d_packet_len_key = pmt::intern("packet_len");
      d_tx_time_key = pmt::intern("tx_time");
      d_burst_mode = burst_mode;
      d_in_burst = false;
      d_burst_samples_remaining = 0;
      d_lead_time_us = lead_time_us;
      d_full = reference_time_full;
      d_frac = reference_time_frac;
      d_static_bits = difi::STANDARD_DATA_STATIC_BITS;
      d_samples_per_packet = samples_per_packet;
      d_time_adj = (double)d_samples_per_packet / samp_rate;
      d_data_len = samples_per_packet * s_bytes_per_sample;

      m_context = difi::make_default_context<V>(
          static_cast<double>(samp_rate), s_bit_depth_val, d_stream_number, reference_time_full, reference_time_frac);
      m_context_buffer.resize(m_context.packet_size);

      m_class_id = m_context.class_id ^ 1;
      m_data_packet_buffer.resize(difi::DIFI_HEADER_SIZE + d_data_len);
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    void difi_sink_cpp_impl<V, Transport, BitDepth>::reanchor_timestamp()
    {
      auto now = std::chrono::system_clock::now();
      auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(now);
      auto frac_duration = now - now_s;

      uint32_t full = static_cast<uint32_t>(now_s.time_since_epoch().count());
      uint64_t frac = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(frac_duration).count()) * 1000ULL;

      frac += static_cast<uint64_t>(d_lead_time_us) * 1000000ULL;
      if (frac >= difi::PICO_CONVERSION) {
        full += static_cast<uint32_t>(frac / difi::PICO_CONVERSION);
        frac = frac % difi::PICO_CONVERSION;
      }

      d_full = full;
      d_frac = frac;
      d_pcks_since_last_reference = 0;
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    void difi_sink_cpp_impl<V, Transport, BitDepth>::flush_current_packet()
    {
      if (d_current_buff_idx <= 0) {
        return;
      }
      std::fill(m_data_packet_buffer.begin() + difi::DIFI_HEADER_SIZE + d_current_buff_idx,
                m_data_packet_buffer.end(), 0);

      if (d_packet_count % d_contex_packet_interval == 0) {
        send_context();
      }

      auto to_send = pack_data();
      if (m_transport) {
        m_transport->send(to_send.data(), to_send.size());
      }

      d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
      d_current_buff_idx = 0;
      d_pcks_since_last_reference++;
      d_packet_count++;
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    int difi_sink_cpp_impl<V, Transport, BitDepth>::work(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items)
    {
      const sample_t *in = reinterpret_cast<const sample_t*>(input_items[0]);
      auto abs_offset = this->nitems_read(0);

      std::vector<tag_t> tags;
      this->get_tags_in_range(tags, 0, abs_offset, abs_offset + noutput_items);
      std::sort(tags.begin(), tags.end(), [](const tag_t &a, const tag_t &b) {
        return a.offset < b.offset;
      });
      size_t tag_idx = 0;

      for(int i = 0; i < noutput_items; i++)
      {
        uint64_t current_offset = abs_offset + i;
        bool burst_started_here = false;
        bool burst_ended_here = false;
        bool has_explicit_time = false;

        while (tag_idx < tags.size() && tags[tag_idx].offset == current_offset) {
          const auto &tag = tags[tag_idx];
          if (pmt::eqv(d_tx_sob_key, tag.key)) {
            d_burst_mode = true;
            d_in_burst = true;
            burst_started_here = true;
          } else if (pmt::eqv(d_packet_len_key, tag.key)) {
            d_burst_mode = true;
            d_in_burst = true;
            burst_started_here = true;
            d_burst_samples_remaining = pmt::to_uint64(tag.value);
          } else if (pmt::eqv(d_tx_eob_key, tag.key)) {
            burst_ended_here = true;
          } else if (pmt::eqv(d_tx_time_key, tag.key)) {
            if (pmt::is_tuple(tag.value) && pmt::length(tag.value) >= 2) {
              d_full = static_cast<uint32_t>(pmt::to_uint64(pmt::tuple_ref(tag.value, 0)));
              d_frac = static_cast<uint64_t>(pmt::to_double(pmt::tuple_ref(tag.value, 1)) * difi::PICO_CONVERSION);
              d_pcks_since_last_reference = 0;
              has_explicit_time = true;
            }
          }
          tag_idx++;
        }

        if (burst_started_here && !has_explicit_time) {
          reanchor_timestamp();
        }

        // Drop/skip idle samples when burst mode is active
        if (d_burst_mode && !d_in_burst && d_burst_samples_remaining == 0) {
          continue;
        }

        this->pack_sample(in[i]);
        d_current_buff_idx += s_bytes_per_sample;

        if (d_burst_samples_remaining > 0) {
          d_burst_samples_remaining--;
          if (d_burst_samples_remaining == 0) {
            burst_ended_here = true;
          }
        }

        if(d_current_buff_idx >= d_data_len)
        {
          if(d_packet_count % d_contex_packet_interval == 0)
              send_context();

          auto to_send = pack_data();
          if(m_transport)
          {
            m_transport->send(to_send.data(), to_send.size());
          }

          d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD;
          d_current_buff_idx = 0;
          d_pcks_since_last_reference++;
          d_packet_count++;
        }

        if (burst_ended_here) {
          flush_current_packet();
          d_in_burst = false;
        }
      }
      return noutput_items;
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    std::span<const uint8_t> difi_sink_cpp_impl<V, Transport, BitDepth>::pack_data()
    {
      auto [full, frac] = add_frac_full();
      uint32_t header = d_static_bits ^ (static_cast<uint32_t>(d_pkt_n) << 16) ^ ((d_data_len + difi::DIFI_HEADER_SIZE) / 4);
      difi::pack_u32(&m_data_packet_buffer[0], header);
      difi::pack_u32(&m_data_packet_buffer[4], d_stream_number);
      difi::pack_u64(&m_data_packet_buffer[8], m_class_id);
      difi::pack_u32(&m_data_packet_buffer[16], full);
      difi::pack_u64(&m_data_packet_buffer[20], frac);
      return std::span<const uint8_t>(m_data_packet_buffer);
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    void difi_sink_cpp_impl<V, Transport, BitDepth>::send_context()
    {
      if constexpr (V == difi::context_version::standard) {
        auto [full, frac] = add_frac_full();
        m_context.full = full;
        m_context.frac = frac;
      }
      difi::pack_context(m_context, d_context_packet_count, d_stream_number, std::span<uint8_t>(m_context_buffer));
      if (m_transport) {
        m_transport->send(m_context_buffer.data(), m_context_buffer.size());
      }
      d_context_packet_count = (d_context_packet_count + 1) % difi::VITA_PKT_MOD;
    }

    template <
        difi::context_version V,
        sink_transport Transport,
        difi::bit_depth BitDepth
    >
    requires valid_bit_depth<BitDepth>
    std::pair<uint32_t, uint64_t> difi_sink_cpp_impl<V, Transport, BitDepth>::add_frac_full()
    {
      return difi::advance_timestamp(
          d_full, d_frac,
          d_pcks_since_last_reference * d_samples_per_packet,
          d_full_samp);
    }

    template class difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_8>;
    template class difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_8>;

    template class difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_8>;
    template class difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_8>;

    template class difi_sink_cpp_impl<difi::context_version::standard, udp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp_impl<difi::context_version::standard, udp_client, difi::bit_depth::bits_8>;
    template class difi_sink_cpp_impl<difi::context_version::standard, tcp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp_impl<difi::context_version::standard, tcp_client, difi::bit_depth::bits_8>;

    template class difi_sink_cpp_impl<difi::context_version::alternative, udp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp_impl<difi::context_version::alternative, udp_client, difi::bit_depth::bits_8>;
    template class difi_sink_cpp_impl<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_16>;
    template class difi_sink_cpp_impl<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_8>;

  } /* namespace difi */
} /* namespace gr */

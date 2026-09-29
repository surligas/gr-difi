// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.

#include <gnuradio/io_signature.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include "difi_sink_cpp_impl.h"

#include "tcp_client.hpp"
#include "udp_client.hpp"

namespace gr {
  namespace difi {

    template <class T>
    typename difi_sink_cpp<T>::sptr
    difi_sink_cpp<T>::make(u_int32_t reference_time_full, u_int64_t reference_time_frac, std::string ip_addr, uint32_t port, uint8_t socket_type,
                          bool mode, uint32_t samples_per_packet, int stream_number, u_int64_t samp_rate,
                          int context_interval, int context_pack_size, int bit_depth,
                          int scaling, float gain, gr_complex offset, float max_iq, float min_iq)
    {
      return gnuradio::make_block_sptr<difi_sink_cpp_impl<T>>(reference_time_full, reference_time_frac, ip_addr, port, socket_type, mode,
                                                              samples_per_packet, stream_number, samp_rate, context_interval, context_pack_size, bit_depth,
                                                              scaling, gain, offset, max_iq, min_iq);
    }

    template <class T>
    difi_sink_cpp_impl<T>::difi_sink_cpp_impl(u_int32_t reference_time_full, u_int64_t reference_time_frac, std::string ip_addr,
                                              uint32_t port, uint8_t socket_type, bool mode, uint32_t samples_per_packet, int stream_number,
                                              u_int64_t samp_rate, int context_interval, int context_pack_size, int bit_depth,
                                              int scaling, float gain, gr_complex offset, float max_iq, float min_iq)
      : gr::sync_block("difi_sink_cpp_impl",
              gr::io_signature::make(1, 1, sizeof(T)),
              gr::io_signature::make(0, 0, 0)),
              d_stream_number(int(stream_number)),
              d_full_samp(samp_rate),
              d_pkt_n(0),
              d_current_buff_idx(0),
              d_pcks_since_last_reference(0),
              d_is_paired_mode(mode),
              d_packet_count(0),
              d_context_packet_count(0),
              d_context_packet_size(context_pack_size),
              d_contex_packet_interval(context_interval)

    {
      if (socket_type == 1) {
        m_transport = std::make_unique<tcp_client>(ip_addr, static_cast<uint16_t>(port));
      } else {
        m_transport = std::make_unique<udp_client>(ip_addr, static_cast<uint16_t>(port));
      }

      if (samples_per_packet < 2)
      {
        GR_LOG_ERROR(this->d_logger, "samples per packet cannot be less than 2, (to ensure one 32 bit word is filled)");
        throw std::runtime_error("samples per packet cannot be less than 2, (to ensure one 32 bit word is filled)");
      }
      d_context_key = pmt::intern("context");
      d_pkt_n_key = pmt::intern("pck_n");
      d_static_change_key = pmt::intern("static_change");
      d_tx_sob_key = pmt::intern("tx_sob");
      d_tx_eob_key = pmt::intern("tx_eob");
      d_packet_len_key = pmt::intern("packet_len");
      d_tx_time_key = pmt::intern("tx_time");
      d_burst_mode = false;
      d_in_burst = false;
      d_burst_samples_remaining = 0;
      d_lead_time_us = 10000;
      d_full = reference_time_full;
      d_frac = reference_time_frac;
      d_static_bits = 0x18e00000; // header bits 31-20 must be 0x18e (posix), 0x18a (gps), or 0x186 (utc)
      if(context_pack_size == 72)// this check is a temporary work around for a non-compliant hardware device
      {
        d_context_static_bits = 0x49000000;// header bits 31-20 must be 0x49e (posix), 0x49a (gps), or 0x496 (utc)
      }
      else {
        d_context_static_bits = 0x49e00000;// header bits 31-20 must be 0x49e (posix), 0x49a (gps), or 0x496 (utc)
      }
      d_unpack_idx_size = bit_depth == 8 ? 1 : 2;
      d_samples_per_packet = samples_per_packet;
      d_time_adj = (double)d_samples_per_packet / samp_rate;
      d_data_len = samples_per_packet * d_unpack_idx_size * 2;
      u_int32_t tmp_header_data = d_static_bits ^ d_pkt_n << 16 ^ (d_data_len + difi::DIFI_HEADER_SIZE) / 4;
      u_int32_t tmp_header_context = d_context_static_bits ^ d_context_packet_count << 16 ^ (context_pack_size  / 4);
      u_int64_t d_class_id;
      if(context_pack_size == 72)// this check is a temporary work around for a non-compliant hardware device
      {
        d_class_id = d_alt_oui << 32; // This is the OUI that is required for the non-compliant hardware device
      } else {
        d_class_id = d_oui << 32;
      }
      u_int64_t d_context_class_id = d_class_id ^ 1;
      d_raw.resize(difi::DIFI_HEADER_SIZE);
      pack_u32(&d_raw[0], tmp_header_data);
      pack_u32(&d_raw[4], d_stream_number);
      int idx = 0;
      pack_u64(&d_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], d_class_id);
      pack_u32(&d_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0);
      pack_u64(&d_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0);

      //pack context for standalone mode only
      d_context_raw.resize(context_pack_size);
      pack_u32(&d_context_raw[0], tmp_header_context);
      pack_u32(&d_context_raw[4], d_stream_number);
      idx = 0;
      pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], d_context_class_id);
      // this is static since only 8 or 16 bit signed complex cartesian is supported for now and item packing is always link efficient
      // see 2.2.2 Standard Flow Signal Context Packet in the DIFI spec for complete information
      u_int64_t data_payload_format = bit_depth == 8 ? difi::EIGHT_BIT_SIGNED_CART_LINK_EFF : difi::SIXTEEN_BIT_SIGNED_CART_LINK_EFF;

      u_int32_t state_and_event_id =difi::DEFAULT_STATE_AND_EVENTS; // default no events or state values. See 7.1.5.17 The State and Event Indicator Field of the VITA spec
      u_int64_t to_vita_bw = u_int64_t(samp_rate * .8) << 20; // no fractional bw or samp rate supported in gnuradio, see 2.2.2 Standard Flow Signal Context Packet for bandwidth information
      u_int64_t to_vita_if_band_offset= int64_t(samp_rate * -0.1) << 20; //generic IF band offset (1Hz resolution)
      u_int64_t to_vita_samp_rate = samp_rate << 20;
      u_int64_t to_vita_if_ref_freq = 100000000UL << 20; //generic 100MHz IF reference frequency
      u_int64_t to_vita_rf_ref_freq = 7500000000UL << 20; //generic 7.5GHz RF reference frequency
      u_int64_t to_vita_ref_level = 20 << 7; //generic 20dBm reference level
      double rf_gain_dB=14.2, if_gain_dB=-1.3; //generic two-stage (RF then IF) gains/attenuations
      int32_t to_vita_rf_gain = rf_gain_dB * (1<<7);
      int32_t to_vita_if_gain = if_gain_dB * (1<<7);
      int32_t to_vita_gain = to_vita_if_gain << 16 ^ to_vita_rf_gain;
      double delay_sec = 1e-5; //generic 10 microsecond timestamp adjustment
      int64_t to_vita_delay = delay_sec * 1e15; //convert to femtoseconds
      if(context_pack_size == 72)// this check is a temporary work around for a non-compliant hardware device
      {
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], 966885376U);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], to_vita_bw);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], 0);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], 0);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], 0);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], to_vita_samp_rate);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], state_and_event_id);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_ALT_OFFSETS[idx++]], data_payload_format);

      }
      else
      {
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0xfbb98000u);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], 0x64u);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_bw);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_if_ref_freq);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_rf_ref_freq);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_if_band_offset);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_ref_level);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_gain);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_samp_rate);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], to_vita_delay);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], d_full);
        pack_u32(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], state_and_event_id);
        pack_u64(&d_context_raw[difi::CONTEXT_PACKET_OFFSETS[idx++]], data_payload_format);
      }
      d_out_buf.resize(d_data_len);

      d_scaling_mode = scaling;

      if(d_scaling_mode == 1){
        //manual
        d_gain = gain;
        d_offset = offset;
      }
      else if(d_scaling_mode == 2){
          //min-max
          int full_scale = (1 << bit_depth) - 1;
          float EPSILON = 0.0001;
          if ((max_iq - min_iq) < EPSILON){
            GR_LOG_ERROR(this->d_logger, "(max_iq - min_iq) too small or is negative, bailing to avoid numerical issues!");
            throw std::runtime_error("(max_iq - min_iq) too small or is negative, bailing to avoid numerical issues!");
          }
          d_gain = float(full_scale) / (max_iq - min_iq);
          d_offset = gr_complex(-1.0 * ((max_iq - min_iq) / 2 + min_iq),-1.0 * ((max_iq - min_iq) / 2 + min_iq));
      }
    }

    template <class T>
    difi_sink_cpp_impl<T>::~difi_sink_cpp_impl() = default;

    template <class T>
    void difi_sink_cpp_impl<T>::reanchor_timestamp()
    {
      auto now = std::chrono::system_clock::now();
      auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(now);
      auto frac_duration = now - now_s;
      auto frac_ps = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::pico>>(frac_duration).count();
      uint32_t full = static_cast<uint32_t>(now_s.time_since_epoch().count());
      uint64_t frac = static_cast<uint64_t>(frac_ps);

      frac += static_cast<uint64_t>(d_lead_time_us) * 1000000ULL;
      if (frac >= difi::PICO_CONVERSION) {
        full += static_cast<uint32_t>(frac / difi::PICO_CONVERSION);
        frac = frac % difi::PICO_CONVERSION;
      }

      d_full = full;
      d_frac = frac;
      d_pcks_since_last_reference = 0;
    }

    template <class T>
    void difi_sink_cpp_impl<T>::flush_current_packet()
    {
      if (d_current_buff_idx <= 0) {
        return;
      }
      std::fill(d_out_buf.begin() + d_current_buff_idx, d_out_buf.end(), 0);

      if (!d_is_paired_mode && d_packet_count % d_contex_packet_interval == 0) {
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

    template <class T>
    int difi_sink_cpp_impl<T>::work(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items)
    {

      const T *in = reinterpret_cast<const T*>(input_items[0]);
      auto abs_offset = this->nitems_read(0);

      if(d_is_paired_mode)
      {
        process_tags(noutput_items);
      }

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

        gr_complex in_val = gr_complex(in[i].real(), in[i].imag());
        if(d_scaling_mode > 0){
            in_val = (in_val + d_offset) * d_gain;
        }
        this->pack_T(in_val);
        d_current_buff_idx += 2 * d_unpack_idx_size;

        if (d_burst_samples_remaining > 0) {
          d_burst_samples_remaining--;
          if (d_burst_samples_remaining == 0) {
            burst_ended_here = true;
          }
        }

        if(d_current_buff_idx >= d_out_buf.size())
        {
          if(!d_is_paired_mode and d_packet_count % d_contex_packet_interval == 0)
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

    template <class T>
    void difi_sink_cpp_impl<T>::process_tags(int noutput_items)
    {
      auto abs_offset = this->nitems_read(0);
      std::vector<tag_t> tags;
      this->get_tags_in_range(tags, 0, abs_offset, abs_offset + noutput_items);
      for(tag_t tag : tags)
      {
        if(pmt::eqv(d_context_key, tag.key))
        {
          auto raw = pmt::dict_ref(tag.value, pmt::intern("raw"), pmt::get_PMT_NIL());;
          std::vector<int8_t> to_send = pmt::s8vector_elements(raw);
          d_raw = to_send;
          if(m_transport)
          {
            m_transport->send(to_send.data(), to_send.size());
          }

          std::copy(to_send.begin(), to_send.begin() + difi::DIFI_HEADER_SIZE, d_raw.begin());
          d_full = u_int32_t(pmt::to_long(pmt::dict_ref(tag.value, pmt::mp("full"), pmt::get_PMT_NIL())));
          d_frac = pmt::to_uint64(pmt::dict_ref(tag.value, pmt::mp("frac"), pmt::get_PMT_NIL()));
          d_stream_number = pmt::to_uint64(pmt::dict_ref(tag.value, pmt::mp("stream_num"), pmt::get_PMT_NIL()));
          d_pcks_since_last_reference = 0;
        }
        else if(pmt::eqv(d_pkt_n_key, tag.key))
        {
          d_pkt_n = (d_pkt_n + 1) % difi::VITA_PKT_MOD; // missed packet, so account for this
          d_full = u_int32_t(pmt::to_long(pmt::dict_ref(tag.value, pmt::mp("full"), pmt::get_PMT_NIL())));
          d_frac = pmt::to_uint64(pmt::dict_ref(tag.value, pmt::mp("frac"), pmt::get_PMT_NIL()));
          d_data_len = pmt::to_uint64(pmt::dict_ref(tag.value, pmt::mp("data_len"), pmt::get_PMT_NIL())) - difi::DIFI_HEADER_SIZE;
          if (d_data_len % (2 * d_unpack_idx_size) != 0)
          {
            GR_LOG_WARN(this->d_logger, "data len cannot fit an integer number of samples, something is misconfigured");
          }
          d_samples_per_packet = d_data_len / (2 * d_unpack_idx_size);
          d_out_buf.clear(); // clear the data since we missed samples, start fresh
          d_out_buf.resize(d_data_len);
          d_current_buff_idx = 0;
          d_time_adj = (double)d_samples_per_packet / d_full_samp;
          d_pcks_since_last_reference = 0;
        }
        else if(pmt::eqv(d_static_change_key, tag.key))
        {
          d_static_bits = pmt::to_uint64(tag.value);
        }
      }
    }
    template <class T>
    std::vector<int8_t> difi_sink_cpp_impl<T>::pack_data()
    {
      std::vector<int8_t> to_send(difi::DIFI_HEADER_SIZE + d_out_buf.size());
      u_int32_t full;
      u_int64_t frac;
      std::tie(full, frac) = add_frac_full();
      u_int32_t header = d_static_bits ^ d_pkt_n << 16 ^ (d_data_len + difi::DIFI_HEADER_SIZE) / 4;
      pack_u32(&to_send[0], header);
      std::copy(d_raw.begin() + 4, d_raw.begin() + 16, to_send.begin() + 4);
      pack_u32(&to_send[16], full);
      pack_u64(&to_send[20], frac);
      std::copy(d_out_buf.begin(), d_out_buf.end(), to_send.begin() + difi::DIFI_HEADER_SIZE);
      return to_send;
    }
    template <class T>
    void difi_sink_cpp_impl<T>::send_context()
    {
        if(d_context_packet_size == 108)// this check is a temporary work around for a non-compliant hardware device
        {
            u_int32_t full;
            u_int64_t frac;
            std::tie(full, frac) = add_frac_full();
            pack_u32(&d_context_raw[16], full);
            pack_u64(&d_context_raw[20], frac);
        }
        u_int32_t header = d_context_static_bits ^ d_context_packet_count << 16 ^ (d_context_packet_size / 4);
        pack_u32(&d_context_raw[0], header);
        if(m_transport)
        {
          m_transport->send(d_context_raw.data(), d_context_raw.size());
        }
        d_context_packet_count = (d_context_packet_count + 1) % difi::VITA_PKT_MOD;
    }

    template <class T>
    std::tuple<u_int32_t, u_int64_t> difi_sink_cpp_impl<T>::add_frac_full()
    {
      auto frac = d_frac;
      auto full = d_full;
      auto time_adj = d_time_adj * d_pcks_since_last_reference;
      frac += u_int64_t((time_adj - u_int64_t(time_adj)) * difi::PICO_CONVERSION);
      if(frac >= difi::PICO_CONVERSION)
      {
          frac = d_frac % difi::PICO_CONVERSION;
          full += 1;
      }
      full += u_int32_t(time_adj);
      return std::make_tuple(full, frac);
    }
    template <class T>
    void difi_sink_cpp_impl<T>::pack_T(T val)
    {
      auto re = (static_cast<int16_t>(val.real()));
      re = htons(re);
      auto im = (static_cast<int16_t>(val.imag()));
      im = htons(im);
      memcpy(&d_out_buf[d_current_buff_idx], &re, d_unpack_idx_size);
      memcpy(&d_out_buf[d_current_buff_idx + d_unpack_idx_size], &im, d_unpack_idx_size);
    }

    template class difi_sink_cpp<gr_complex>;
    template class difi_sink_cpp<std::complex<char>>;

  } /* namespace difi */
} /* namespace gr */


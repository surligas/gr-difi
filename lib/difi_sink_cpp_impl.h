// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.



#ifndef INCLUDED_DIFI_SINK_CPP_IMPL_H
#define INCLUDED_DIFI_SINK_CPP_IMPL_H

#include <difi/difi_sink_cpp.h>
#include <difi/difi.hpp>
#include <arpa/inet.h>
#include <memory>
#include <span>
#include <vector>

namespace gr {
  namespace difi {

    template <
        difi::context_version V = difi::context_version::standard,
        sink_transport Transport = udp_client,
        difi::bit_depth BitDepth = difi::bit_depth::bits_16
    >
    requires valid_bit_depth<BitDepth>
    class difi_sink_cpp_impl : public difi_sink_cpp<V, Transport, BitDepth>
    {
     public:
        using sample_t = sink_sample_type_t<BitDepth>;

     private:
        std::span<const uint8_t> pack_data(size_t payload_bytes);
        void send_context();
        std::pair<uint32_t, uint64_t> add_frac_full();
        void reanchor_timestamp();

        difi::context<V> m_context;
        std::vector<uint8_t> m_context_buffer;
        std::vector<uint8_t> m_data_buffer;
        uint64_t m_class_id;

        std::unique_ptr<Transport> m_transport;

        static constexpr uint32_t s_bytes_per_sample = sizeof(sample_t);
        static constexpr int s_bit_depth_val = static_cast<int>(BitDepth);

        int d_stream_number;
        uint32_t d_full_samp;
        std::string m_length_tag_name;
        pmt::pmt_t m_length_tag_key;
        uint64_t d_burst_samples_remaining;
        uint32_t d_lead_time_us;
        uint32_t d_full;
        uint64_t d_frac;
        uint32_t d_data_len;
        uint8_t d_pkt_n;
        int32_t d_static_bits;
        uint64_t d_samples_since_last_reference;
        uint64_t d_contex_packet_interval;
        uint64_t d_packet_count;
        uint32_t d_context_packet_count;
        uint32_t d_samples_per_packet;

     public:
      difi_sink_cpp_impl(std::string ip_addr, uint16_t port, uint64_t samp_rate,
                        uint32_t samples_per_packet, int stream_number = 0,
                        int context_interval = 100, uint32_t reference_time_full = 0,
                        uint64_t reference_time_frac = 0,
                        const std::string& length_tag_name = "", uint32_t lead_time_us = 10000);
      ~difi_sink_cpp_impl() override = default;

      // Where all the action really happens
      int work(
              int noutput_items,
              gr_vector_const_void_star &input_items,
              gr_vector_void_star &output_items
      ) override;

      void set_length_tag_name(const std::string& tag_name) override {
        m_length_tag_name = tag_name;
        m_length_tag_key = tag_name.empty() ? pmt::PMT_NIL : pmt::intern(tag_name);
      }
      std::string length_tag_name() const override { return m_length_tag_name; }
      bool burst_mode() const override { return !m_length_tag_name.empty(); }
      void set_lead_time_us(uint32_t lead_time_us) override { d_lead_time_us = lead_time_us; }
      uint32_t lead_time_us() const override { return d_lead_time_us; }

      difi::context_variant context() const override { return m_context; }
      void set_context(const difi::context_variant& ctx) override
      {
        if (std::holds_alternative<difi::context<V>>(ctx)) {
          m_context = std::get<difi::context<V>>(ctx);
        } else {
          throw std::invalid_argument("Context version mismatch");
        }
      }

      const difi::context<V>& typed_context() const { return m_context; }
      void set_typed_context(const difi::context<V>& ctx) { m_context = ctx; }
    };

  } // namespace difi
} // namespace gr

#endif /* INCLUDED_DIFI_SINK_CPP_IMPL_H */


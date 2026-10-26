// -*- c++ -*-
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.



#ifndef INCLUDED_DIFI_SINK_CPP_IMPL_H
#define INCLUDED_DIFI_SINK_CPP_IMPL_H

#include <difi/difi_sink_cpp.h>
#include <difi/difi.hpp>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
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

        void rx_loop();
        void start_rx_thread();
        void stop_rx_thread();
        void wait_rate_control(uint32_t num_samples);
        void update_estimated_fill(std::chrono::steady_clock::time_point now);
        void check_and_publish_flow_control(const difi::flow_control& fc);

        difi::context<V> m_context;
        std::vector<uint8_t> m_context_buffer;
        std::vector<uint8_t> m_data_buffer;
        uint64_t m_class_id;

        std::unique_ptr<Transport> m_transport;

        std::atomic<bool> m_rate_control{false};
        std::atomic<float> m_max_buffer_fill_pct{50.0f};
        std::thread m_rx_thread;
        std::atomic<bool> m_rx_running{false};
        mutable std::mutex m_fc_mutex;
        std::condition_variable m_fc_cv;
        std::atomic<bool> m_first_fc_received{false};
        difi::flow_control m_last_fc{};
        double m_est_fill_bytes{0.0};
        std::chrono::steady_clock::time_point m_last_update_time{};

        std::atomic<float> m_buffer_fill_delta{1.0f};
        bool m_fc_published{false};
        float m_last_published_pct{0.0f};
        bool m_last_published_overflow{false};
        bool m_last_published_nearly_full{false};
        bool m_last_published_nearly_empty{false};
        bool m_last_published_underflow{false};

        static constexpr uint32_t s_bytes_per_sample = sizeof(sample_t);
        static constexpr int s_bit_depth_val = static_cast<int>(BitDepth);

        int d_stream_number;
        double d_samp_rate;
        std::string m_length_tag_name;
        pmt::pmt_t m_length_tag_key;
        uint64_t d_burst_samples_remaining;
        uint32_t d_lead_time_us;
        difi::tsi_type m_time_source;
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
      difi_sink_cpp_impl(std::string ip_addr, uint16_t port, double samp_rate,
                        uint32_t samples_per_packet, int stream_number = 0,
                        int context_interval = 100,
                        const std::string& length_tag_name = "", uint32_t lead_time_us = 10000,
                        double rf_freq = 0.0, double if_freq = 0.0, double bandwidth = 0.0,
                        float rf_gain = 0.0f, float if_gain = 0.0f, float ref_level = 0.0f,
                        double if_band_offset = 0.0,
                        difi::reference_point ref_point = difi::reference_point::if_converter,
                        difi::tsi_type time_source = difi::tsi_type::posix,
                        bool rate_control = false,
                        float max_buffer_fill_pct = 50.0f,
                        float buffer_fill_delta = 1.0f);
      ~difi_sink_cpp_impl() override;

      bool start() override;
      bool stop() override;

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

      void set_rf_freq(double freq) override { m_context.rf_ref_freq = freq; }
      double rf_freq() const override { return m_context.rf_ref_freq; }
      void set_center_freq(double freq) override { set_rf_freq(freq); }
      double center_freq() const override { return rf_freq(); }

      void set_if_freq(double freq) override { m_context.if_ref_freq = freq; }
      double if_freq() const override { return m_context.if_ref_freq; }

      void set_bandwidth(double bw) override
      {
          m_context.bandwidth = (bw > 0.0) ? bw : d_samp_rate;
      }
      double bandwidth() const override { return m_context.bandwidth; }

      void set_rf_gain(float gain) override
      {
          if constexpr (V == difi::context_version::standard) {
              m_context.rf_gain = gain;
          }
      }
      float rf_gain() const override
      {
          if constexpr (V == difi::context_version::standard) {
              return m_context.rf_gain;
          } else {
              return 0.0f;
          }
      }
      void set_gain(float gain) override { set_rf_gain(gain); }
      float gain() const override { return rf_gain(); }

      void set_if_gain(float gain) override
      {
          if constexpr (V == difi::context_version::standard) {
              m_context.if_gain = gain;
          }
      }
      float if_gain() const override
      {
          if constexpr (V == difi::context_version::standard) {
              return m_context.if_gain;
          } else {
              return 0.0f;
          }
      }

      void set_ref_level(float level) override
      {
          if constexpr (V == difi::context_version::standard) {
              m_context.ref_level = level;
          }
      }
      float ref_level() const override
      {
          if constexpr (V == difi::context_version::standard) {
              return m_context.ref_level;
          } else {
              return 0.0f;
          }
      }

      void set_if_band_offset(double offset) override { m_context.if_band_offset = offset; }
      double if_band_offset() const override { return m_context.if_band_offset; }

      void set_ref_point(difi::reference_point rp) override
      {
          if constexpr (V == difi::context_version::standard) {
              m_context.ref_point = rp;
          }
      }
      difi::reference_point ref_point() const override
      {
          if constexpr (V == difi::context_version::standard) {
              return m_context.ref_point;
          } else {
              return difi::reference_point::unspecified;
          }
      }

      void set_time_source(difi::tsi_type tsi) override
      {
          m_time_source = tsi;
          d_static_bits = difi::make_data_static_bits(m_time_source);
          if constexpr (V == difi::context_version::standard) {
              m_context.tsi = m_time_source;
          }
      }
      difi::tsi_type time_source() const override { return m_time_source; }

      void set_rate_control(bool enabled) override
      {
          if (m_rate_control.load() == enabled) return;
          m_rate_control.store(enabled);
          if (enabled) {
              start_rx_thread();
          } else {
              stop_rx_thread();
          }
      }
      bool rate_control() const override { return m_rate_control.load(); }

      void set_max_buffer_fill_pct(float pct) override
      {
          m_max_buffer_fill_pct.store(pct);
          m_fc_cv.notify_all();
      }
      float max_buffer_fill_pct() const override { return m_max_buffer_fill_pct.load(); }

      void set_buffer_fill_delta(float delta) override { m_buffer_fill_delta.store(delta); }
      float buffer_fill_delta() const override { return m_buffer_fill_delta.load(); }

      float last_buffer_fill_pct() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.buffer_fill_pct();
      }
      uint16_t last_buffer_level() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.buffer_level;
      }
      uint64_t device_buffer_size() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.buffer_size;
      }
      bool device_nearly_full() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.nearly_full;
      }
      bool device_nearly_empty() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.nearly_empty;
      }
      bool device_overflow() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.overflow;
      }
      bool device_underflow() const override
      {
          std::lock_guard<std::mutex> lock(m_fc_mutex);
          return m_last_fc.underflow;
      }

      const difi::context<V>& typed_context() const { return m_context; }
      void set_typed_context(const difi::context<V>& ctx) { m_context = ctx; }
    };

  } // namespace difi
} // namespace gr

#endif /* INCLUDED_DIFI_SINK_CPP_IMPL_H */


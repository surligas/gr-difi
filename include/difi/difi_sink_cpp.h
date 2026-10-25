// -*- c++ -*- //
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.
//

#ifndef INCLUDED_DIFI_SINK_CPP_H
#define INCLUDED_DIFI_SINK_CPP_H

#include <difi/api.h>
#include <difi/difi.hpp>
#include <difi/tcp_client.hpp>
#include <difi/udp_client.hpp>
#include <concepts>
#include <memory>
#include <string>
#include <gnuradio/sync_block.h>

namespace gr {
  namespace difi {

    template <class Transport>
    concept sink_transport = std::same_as<Transport, udp_client> ||
                             std::same_as<Transport, tcp_client>;

    template <difi::bit_depth B>
    concept valid_bit_depth = (B == difi::bit_depth::bits_8 ||
                               B == difi::bit_depth::bits_16);

    template <difi::bit_depth B>
    struct sink_sample_type;

    template <>
    struct sink_sample_type<difi::bit_depth::bits_8> {
        using type = std::complex<char>;
    };

    template <>
    struct sink_sample_type<difi::bit_depth::bits_16> {
        using type = std::complex<short>;
    };

    template <difi::bit_depth B>
    using sink_sample_type_t = typename sink_sample_type<B>::type;

    /**
     * @brief Abstract base class for the DIFI sink block.
     *
     * Provides common virtual methods for context inspection, burst mode,
     * and GNU Radio sync_block lifecycle.
     */
    class DIFI_API difi_sink_base : virtual public gr::sync_block
    {
     public:
      typedef std::shared_ptr<difi_sink_base> sptr;
      virtual ~difi_sink_base() = default;

      virtual void set_length_tag_name(const std::string& tag_name) = 0;
      virtual std::string length_tag_name() const = 0;
      virtual bool burst_mode() const = 0;
      virtual void set_lead_time_us(uint32_t lead_time_us) = 0;
      virtual uint32_t lead_time_us() const = 0;

      virtual difi::context_variant context() const = 0;
      virtual void set_context(const difi::context_variant& ctx) = 0;

      virtual void set_rf_freq(double freq) = 0;
      virtual double rf_freq() const = 0;
      virtual void set_center_freq(double freq) = 0;
      virtual double center_freq() const = 0;

      virtual void set_if_freq(double freq) = 0;
      virtual double if_freq() const = 0;

      virtual void set_bandwidth(double bw) = 0;
      virtual double bandwidth() const = 0;

      virtual void set_rf_gain(float gain) = 0;
      virtual float rf_gain() const = 0;
      virtual void set_gain(float gain) = 0;
      virtual float gain() const = 0;

      virtual void set_if_gain(float gain) = 0;
      virtual float if_gain() const = 0;

      virtual void set_ref_level(float level) = 0;
      virtual float ref_level() const = 0;

      virtual void set_if_band_offset(double offset) = 0;
      virtual double if_band_offset() const = 0;

      virtual void set_ref_point(difi::reference_point rp) = 0;
      virtual difi::reference_point ref_point() const = 0;

      virtual void set_time_source(difi::tsi_type tsi) = 0;
      virtual difi::tsi_type time_source() const = 0;
      void set_tsi(difi::tsi_type tsi) { set_time_source(tsi); }
      difi::tsi_type tsi() const { return time_source(); }

      virtual void set_rate_control(bool enabled) = 0;
      virtual bool rate_control() const = 0;
      virtual void set_max_buffer_fill_pct(float pct) = 0;
      virtual float max_buffer_fill_pct() const = 0;
      virtual float last_buffer_fill_pct() const = 0;
      virtual uint16_t last_buffer_level() const = 0;
      virtual uint64_t device_buffer_size() const = 0;
      virtual bool device_nearly_full() const = 0;
      virtual bool device_nearly_empty() const = 0;
      virtual bool device_overflow() const = 0;
      virtual bool device_underflow() const = 0;
    };

    /**
     * @brief Templated DIFI sink block.
     *
     * Transmits complex samples as DIFI data packets over UDP or TCP.
     * The input sample type is determined by BitDepth:
     * - bits_8: std::complex<char> (sc8, interleaved chars)
     * - bits_16: std::complex<short> (sc16, interleaved shorts)
     *
     * @tparam V context version (standard or alternative)
     * @tparam Transport transport class (udp_client or tcp_client)
     * @tparam BitDepth bit depth (bits_8 or bits_16)
     */
    template <
        difi::context_version V = difi::context_version::standard,
        sink_transport Transport = udp_client,
        difi::bit_depth BitDepth = difi::bit_depth::bits_16
    >
    requires valid_bit_depth<BitDepth>
    class DIFI_API difi_sink_cpp : public difi_sink_base
    {
     public:
      typedef std::shared_ptr<difi_sink_cpp<V, Transport, BitDepth>> sptr;
      using sample_t = sink_sample_type_t<BitDepth>;

      static difi_sink_base::sptr make(
          std::string ip_addr,
          uint16_t port,
          uint64_t samp_rate,
          uint32_t samples_per_packet,
          int stream_number = 0,
          int context_interval = 100,
          const std::string& length_tag_name = "",
          uint32_t lead_time_us = 10000,
          double rf_freq = 0.0,
          double if_freq = 0.0,
          double bandwidth = 0.0,
          float rf_gain = 0.0f,
          float if_gain = 0.0f,
          float ref_level = 0.0f,
          double if_band_offset = 0.0,
          difi::reference_point ref_point = difi::reference_point::if_converter,
          difi::tsi_type time_source = difi::tsi_type::posix,
          bool rate_control = false,
          float max_buffer_fill_pct = 50.0f);
    };

    typedef difi_sink_base difi_sink_cpp_sc8;
    typedef difi_sink_base difi_sink_cpp_sc16;

  } // namespace difi
} // namespace gr

#endif /* INCLUDED_DIFI_SINK_CPP_H */


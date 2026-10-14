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

      virtual void set_burst_mode(bool enable) = 0;
      virtual bool burst_mode() const = 0;
      virtual void set_lead_time_us(uint32_t lead_time_us) = 0;
      virtual uint32_t lead_time_us() const = 0;

      virtual difi::context_variant context() const = 0;
      virtual void set_context(const difi::context_variant& ctx) = 0;
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
          uint32_t reference_time_full = 0,
          uint64_t reference_time_frac = 0,
          bool burst_mode = false,
          uint32_t lead_time_us = 10000);
    };

    typedef difi_sink_base difi_sink_cpp_sc8;
    typedef difi_sink_base difi_sink_cpp_sc16;

  } // namespace difi
} // namespace gr

#endif /* INCLUDED_DIFI_SINK_CPP_H */


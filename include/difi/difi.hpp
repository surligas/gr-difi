/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <gnuradio/types.h>
#include <difi/api.h>
#include <pmt/pmt.h>

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <bit>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gr::difi {

class difi;

/**
 * @brief Concept constraining complex sample types supported by DIFI.
 */
template <typename T>
concept sample_type = std::same_as<T, gr_complex> || std::same_as<T, std::complex<char>>;

/**
 * @brief Central class for DIFI and VITA 49 protocol operations.
 *
 * Provides header parsing and serialization, context packet encoding and
 * decoding, VITA-49 fixed-point math, sample packing and unpacking with SIMD
 * endianness conversion, timestamp arithmetic, and PMT dictionary conversions
 * for GNU Radio stream tags.
 */
class DIFI_API difi
{
public:
    // --- Nested Types and Enums ---

    /**
     * @brief DIFI packet types according to IEEE-ISTO 4900-2021 and VITA 49.2.
     */
    enum class packet_type : uint8_t {
        unknown = 0, ///< Unknown packet type
        data = 1,    ///< Standard Flow Signal Data Packet
        context = 4, ///< Standard Flow Signal Context Packet
        version = 5  ///< Version Flow Signal Context Packet (DIFI 1.1 / 1.2.1)
    };

    /**
     * @brief Bit depths supported by DIFI.
     */
    enum class bit_depth : uint8_t {
        bits_8 = 8,  ///< 8-bit signed integer samples
        bits_16 = 16 ///< 16-bit signed integer samples
    };

    /**
     * @brief Context packet specification versions.
     */
    enum class context_version : uint8_t {
        standard,   ///< Standard 108-byte context packet (16 fields)
        alternative ///< Alternative 72-byte context packet (9 fields)
    };

    /**
     * @brief Context packet handling behavior when an unexpected context arrives.
     */
    enum class context_behavior : uint8_t {
        throw_exe = 0,          ///< Throw an exception
        ignore = 1,             ///< Silently ignore
        warnings_forward = 2,   ///< Log warning and forward packet
        warnings_no_forward = 3 ///< Log warning and drop packet
    };

    /**
     * @brief Decoded fields from a 28-byte DIFI packet header.
     */
    struct DIFI_API header {
        packet_type type = packet_type::unknown; ///< Packet type
        uint8_t pkt_n = 0;              ///< Modulo-16 packet sequence number (bits 19-16)
        uint16_t packet_size_words = 0; ///< Packet size in 32-bit words (bits 15-0)
        size_t packet_size_bytes = 0;   ///< Packet size in bytes
        uint32_t stream_id = 0;         ///< Stream identifier (word 1)
        uint64_t class_id = 0;          ///< Class identifier (words 2-3)
        uint32_t full = 0;              ///< Integer seconds timestamp (word 4)
        uint64_t frac = 0;              ///< Fractional picoseconds timestamp (words 5-6)
        uint32_t static_bits = 0;       ///< Header static bits (bits 31-20)
        uint32_t raw_header = 0;        ///< Raw header word 0
    };

    // --- Wire Format Constants ---
    static constexpr size_t HEADER_SIZE = 28;
    static constexpr size_t STANDARD_CONTEXT_SIZE = 108;
    static constexpr size_t ALT_CONTEXT_SIZE = 72;
    static constexpr uint8_t PACKET_MOD = 16;
    static constexpr uint64_t PICO_PER_SEC = 1000000000000ULL;

    // Backwards compatibility constants
    static constexpr uint8_t DIFI_HEADER_SIZE = HEADER_SIZE;
    static constexpr uint8_t VITA_PKT_MOD = PACKET_MOD;
    static constexpr uint32_t MS_DATA_VITA_HEADER = 0x18;
    static constexpr uint64_t PICO_CONVERSION = PICO_PER_SEC;
    static constexpr std::array<uint16_t, 16> CONTEXT_PACKET_OFFSETS = {
        8, 16, 20, 28, 32, 36, 44, 52, 60, 68, 72, 76, 84, 92, 96, 100
    };
    static constexpr std::array<uint16_t, 9> CONTEXT_PACKET_ALT_OFFSETS = { 8,  16, 20,
                                                                            28, 36, 44,
                                                                            52, 60, 64 };
    static constexpr uint32_t DATA_START_IDX = 28;
    static constexpr uint64_t DEFAULT_STATE_AND_EVENTS = 2685009920ULL;
    static constexpr uint64_t EIGHT_BIT_SIGNED_CART_LINK_EFF = 0xa00001c700000000ULL;
    static constexpr uint64_t SIXTEEN_BIT_SIGNED_CART_LINK_EFF = 0xa00003cf00000000ULL;
    static constexpr uint64_t STANDARD_OUI = 0x6a621e;
    static constexpr uint64_t ALT_OUI = 0x7c386c;
    static constexpr size_t STANDARD_CONTEXT_PACKET_SIZE = STANDARD_CONTEXT_SIZE;
    static constexpr size_t ALT_CONTEXT_PACKET_SIZE = ALT_CONTEXT_SIZE;
    static constexpr uint32_t STANDARD_DATA_STATIC_BITS = 0x18e00000;
    static constexpr uint32_t STANDARD_CONTEXT_STATIC_BITS = 0x49e00000;
    static constexpr uint32_t ALT_CONTEXT_STATIC_BITS = 0x49000000;

    // --- Context Representation Templates ---

    /**
     * @brief Primary template for context representations.
     *
     * @tparam V context version (standard or alternative)
     */
    template <context_version V = context_version::standard>
    struct context;

    using standard_context = context<context_version::standard>;
    using alternative_context = context<context_version::alternative>;
    using difi_context = standard_context;
    using context_variant = std::variant<standard_context, alternative_context>;
    using difi_header = header;

    // --- Fixed-Point Conversions ---

    /**
     * @brief Converts a 64-bit unsigned VITA fixed-point number to double.
     *
     * @param bits 64-bit unsigned fixed-point value (20 fractional bits)
     * @return double floating-point value
     */
    static double vita_fixed_to_double(uint64_t bits);

    /**
     * @brief Converts a 64-bit signed VITA fixed-point number to double.
     *
     * @param bits 64-bit signed fixed-point value (20 fractional bits)
     * @return double floating-point value
     */
    static double vita_fixed_to_double(int64_t bits);

    /**
     * @brief Converts a double to a 64-bit unsigned VITA fixed-point number.
     *
     * @param val double floating-point value
     * @return 64-bit unsigned fixed-point value (20 fractional bits)
     */
    static uint64_t double_to_vita_fixed(double val);

    /**
     * @brief Converts a 16-bit signed VITA fixed-point number to float.
     *
     * @param bits 16-bit signed fixed-point value (7 fractional bits)
     * @return float value
     */
    static float vita_fixed_to_float(int16_t bits);

    /**
     * @brief Converts a float to a 16-bit signed VITA fixed-point number.
     *
     * @param val float value
     * @return 16-bit signed fixed-point value (7 fractional bits)
     */
    static int16_t float_to_vita_fixed(float val);

    // --- SIMD Endianness Conversion ---

    /**
     * @brief Byte-swaps 16-bit unsigned integers using VOLK SIMD kernels.
     *
     * On little-endian architectures, calls volk_16u_byteswap. On big-endian
     * architectures, this is a compile-time no-op.
     *
     * @param data pointer to 16-bit elements
     * @param num_points number of 16-bit elements to swap
     */
    static void swap_endian_16(uint16_t* data, size_t num_points);

    /**
     * @brief Byte-swaps 16-bit unsigned integers using VOLK SIMD kernels on a span.
     *
     * @param data span of 16-bit elements
     */
    static void swap_endian_16(std::span<uint16_t> data);

    // --- Header Operations ---

    /**
     * @brief Parses a 28-byte DIFI header from a buffer.
     *
     * @param buf pointer to buffer
     * @param len size of buffer in bytes
     * @return decoded header
     */
    static header parse_header(const void* buf, size_t len);

    /**
     * @brief Parses a 28-byte DIFI header from a byte span.
     *
     * @param buf byte span
     * @return decoded header
     */
    static header parse_header(std::span<const uint8_t> buf);

    /**
     * @brief Serializes a 28-byte DIFI header into a buffer.
     *
     * @param buf pointer to output buffer (at least HEADER_SIZE bytes)
     * @param header header details to pack
     */
    static void pack_header(void* buf, const header& header);

    /**
     * @brief Serializes a 28-byte DIFI header into a byte span.
     *
     * @param buf destination byte span
     * @param header header details to pack
     */
    static void pack_header(std::span<uint8_t> buf, const header& header);

    // --- Context Packet Operations ---

    /**
     * @brief Parses a context packet with static version selection.
     *
     * @tparam V context version (standard or alternative)
     * @param buf pointer to packet buffer
     * @param len size of buffer in bytes
     * @return decoded context<V>
     */
    template <context_version V = context_version::standard>
    static context<V> parse_context(const void* buf, size_t len);

    /**
     * @brief Parses a context packet with static version selection from a span.
     *
     * @tparam V context version (standard or alternative)
     * @param buf byte span
     * @return decoded context<V>
     */
    template <context_version V = context_version::standard>
    static context<V> parse_context(std::span<const uint8_t> buf);

    /**
     * @brief Parses a context packet with runtime version dispatch.
     *
     * Selects standard_context for 108-byte packets and alternative_context
     * for 72-byte packets.
     *
     * @param buf pointer to packet buffer
     * @param len size of buffer in bytes
     * @return context_variant containing standard_context or alternative_context
     */
    static context_variant parse_context(const void* buf, size_t len);

    /**
     * @brief Parses a context packet with runtime dispatch from a byte span.
     *
     * @param buf byte span
     * @return context_variant containing standard_context or alternative_context
     */
    static context_variant parse_context(std::span<const uint8_t> buf);

    /**
     * @brief Serializes a typed context packet.
     *
     * @tparam V context version
     * @param ctx context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param packet_size packet size in bytes
     * @return byte vector containing the packed context packet
     */
    template <context_version V>
    static std::vector<uint8_t>
    pack_context(const context<V>& ctx,
                 uint8_t pkt_n,
                 uint32_t stream_id,
                 size_t packet_size = (V == context_version::alternative
                                           ? ALT_CONTEXT_SIZE
                                           : STANDARD_CONTEXT_SIZE));

    /**
     * @brief Serializes a standard context packet.
     *
     * @param ctx standard context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param packet_size packet size in bytes (defaults to 108)
     * @return byte vector containing the packed context packet
     */
    static std::vector<uint8_t> pack_context(const standard_context& ctx,
                                             uint8_t pkt_n,
                                             uint32_t stream_id,
                                             size_t packet_size = STANDARD_CONTEXT_SIZE);

    /**
     * @brief Serializes an alternative context packet.
     *
     * @param ctx alternative context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param packet_size packet size in bytes (defaults to 72)
     * @return byte vector containing the packed context packet
     */
    static std::vector<uint8_t> pack_context(const alternative_context& ctx,
                                             uint8_t pkt_n,
                                             uint32_t stream_id,
                                             size_t packet_size = ALT_CONTEXT_SIZE);

    /**
     * @brief Serializes a context packet variant.
     *
     * @param ctx context variant (standard or alternative)
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @return byte vector containing the packed context packet
     */
    static std::vector<uint8_t>
    pack_context(const context_variant& ctx, uint8_t pkt_n, uint32_t stream_id);

    /**
     * @brief Creates a default typed context packet for standalone sink mode.
     *
     * @tparam V context version
     * @param samp_rate sample rate in Hz
     * @param depth bit depth (8 or 16)
     * @param stream_id stream identifier
     * @param full reference integer timestamp
     * @param frac reference fractional timestamp
     * @return context<V> configured with default values
     */
    template <context_version V = context_version::standard>
    static context<V> make_default_context(double samp_rate,
                                           int depth,
                                           uint32_t stream_id,
                                           uint32_t full = 0,
                                           uint64_t frac = 0);

    /**
     * @brief Creates a default context packet with size-based configuration.
     *
     * Provided for backwards compatibility with legacy callers.
     *
     * @param samp_rate sample rate in Hz
     * @param depth bit depth (8 or 16)
     * @param stream_id stream identifier
     * @param full reference integer timestamp
     * @param frac reference fractional timestamp
     * @param packet_size packet size in bytes (108 standard or 72 alternative)
     * @return standard_context configured with default values
     */
    static standard_context make_default_context(double samp_rate,
                                                 int depth,
                                                 uint32_t stream_id,
                                                 uint32_t full,
                                                 uint64_t frac,
                                                 size_t packet_size);

    // --- Timestamp Arithmetic ---

    /**
     * @brief Advances a DIFI timestamp by a given number of samples.
     *
     * @param full current integer seconds
     * @param frac current fractional picoseconds
     * @param num_samples number of elapsed samples
     * @param samp_rate sample rate in Hz
     * @return std::pair<uint32_t, uint64_t> updated {full, frac}
     */
    static std::pair<uint32_t, uint64_t>
    advance_timestamp(uint32_t full, uint64_t frac, size_t num_samples, double samp_rate);

    // --- Sample Packing and Unpacking ---

    /**
     * @brief Packs complex samples into raw payload bytes with optional scaling.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input complex samples
     * @param num_samples number of complex samples
     * @param payload output buffer
     * @param depth bit depth (8 or 16)
     * @param scaling_mode 0: none, 1: manual (gain & offset)
     * @param gain scalar multiplier for manual scaling
     * @param offset complex offset added prior to gain
     */
    template <sample_type T>
    static void pack_samples(const T* in,
                             size_t num_samples,
                             void* payload,
                             int depth,
                             int scaling_mode = 0,
                             float gain = 1.0f,
                             gr_complex offset = { 0.0f, 0.0f });

    /**
     * @brief Packs complex samples into raw payload span with optional scaling.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input sample span
     * @param payload output payload span
     * @param depth bit depth (8 or 16)
     * @param scaling_mode 0: none, 1: manual (gain & offset)
     * @param gain scalar multiplier for manual scaling
     * @param offset complex offset added prior to gain
     */
    template <sample_type T>
    static void pack_samples(std::span<const T> in,
                             std::span<uint8_t> payload,
                             int depth,
                             int scaling_mode = 0,
                             float gain = 1.0f,
                             gr_complex offset = { 0.0f, 0.0f });

    /**
     * @brief Unpacks complex samples from raw payload bytes.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param payload input payload buffer
     * @param payload_bytes number of payload bytes available
     * @param out output sample buffer
     * @param max_samples maximum number of samples out can hold
     * @param depth bit depth (8 or 16)
     * @return number of complex samples unpacked
     */
    template <sample_type T>
    static size_t unpack_samples(
        const void* payload, size_t payload_bytes, T* out, size_t max_samples, int depth);

    /**
     * @brief Unpacks complex samples from raw payload span into sample span.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param payload input payload span
     * @param out output sample span
     * @param depth bit depth (8 or 16)
     * @return number of complex samples unpacked
     */
    template <sample_type T>
    static size_t
    unpack_samples(std::span<const uint8_t> payload, std::span<T> out, int depth);

    // --- GNU Radio PMT Interoperability ---

    /**
     * @brief Converts standard context packet fields into a GNU Radio PMT dictionary.
     *
     * @param header packet header
     * @param ctx standard context fields
     * @return PMT dictionary
     */
    static pmt::pmt_t context_to_pmt(const header& header, const standard_context& ctx);

    /**
     * @brief Converts alternative context packet fields into a GNU Radio PMT dictionary.
     *
     * @param header packet header
     * @param ctx alternative context fields
     * @return PMT dictionary
     */
    static pmt::pmt_t context_to_pmt(const header& header,
                                     const alternative_context& ctx);

    /**
     * @brief Converts context variant fields into a GNU Radio PMT dictionary.
     *
     * @param header packet header
     * @param ctx context variant
     * @return PMT dictionary
     */
    static pmt::pmt_t context_to_pmt(const header& header, const context_variant& ctx);

    /**
     * @brief Decodes a context packet from a GNU Radio PMT dictionary.
     *
     * @tparam V context version
     * @param dict PMT dictionary
     * @return decoded context<V>
     */
    template <context_version V = context_version::standard>
    static context<V> pmt_to_context(pmt::pmt_t dict);

    /**
     * @brief Decodes a standard context packet from a GNU Radio PMT dictionary.
     *
     * @param dict PMT dictionary
     * @return decoded standard_context
     */
    static standard_context pmt_to_context(pmt::pmt_t dict);

    /**
     * @brief Creates a PMT dictionary for packet metadata stream tags.
     *
     * @param pkt_n packet sequence number
     * @param data_len payload data length in bytes
     * @param full integer seconds timestamp
     * @param frac fractional picoseconds timestamp
     * @return PMT dictionary containing pck_n, data_len, full, and frac
     */
    static pmt::pmt_t
    make_pkt_n_dict(uint8_t pkt_n, size_t data_len, uint32_t full, uint64_t frac);
};

// --- Context Template Specializations ---

/**
 * @brief Specialization for standard 108-byte context packets (16 fields).
 */
template <>
struct DIFI_API difi::context<difi::context_version::standard> {
    uint64_t class_id = 0;                      ///< Class identifier
    uint32_t full = 0;                          ///< Integer seconds timestamp
    uint64_t frac = 0;                          ///< Fractional picoseconds timestamp
    uint32_t cif = 0;                           ///< Context Indicator Field
    uint32_t ref_point = 0;                     ///< Reference point
    double bandwidth = 0.0;                     ///< Bandwidth in Hz
    double if_ref_freq = 0.0;                   ///< IF reference frequency in Hz
    double rf_ref_freq = 0.0;                   ///< RF reference frequency in Hz
    double if_band_offset = 0.0;                ///< IF band offset in Hz
    float ref_level = 0.0f;                     ///< Reference level in dBm
    float rf_gain = 0.0f;                       ///< RF gain in dB
    float if_gain = 0.0f;                       ///< IF gain in dB
    double sample_rate = 0.0;                   ///< Sample rate in Hz
    int64_t timestamp_adjustment = 0;           ///< Timestamp adjustment in femtoseconds
    uint32_t timestamp_calibration_time = 0;    ///< Timestamp calibration time in seconds
    uint32_t state_and_event_indicators = 0;    ///< State and event indicator field
    uint64_t payload_format = 0;                ///< Data packet payload format field
    size_t packet_size = STANDARD_CONTEXT_SIZE; ///< Packet size in bytes (108)
    std::vector<int8_t> raw;                    ///< Raw bytes of the context packet
};

/**
 * @brief Specialization for alternative 72-byte context packets (9 fields).
 */
template <>
struct DIFI_API difi::context<difi::context_version::alternative> {
    uint64_t class_id = 0;                   ///< Class identifier
    uint32_t cif = 0;                        ///< Context Indicator Field
    double bandwidth = 0.0;                  ///< Bandwidth in Hz
    double if_ref_freq = 0.0;                ///< IF reference frequency in Hz
    double rf_ref_freq = 0.0;                ///< RF reference frequency in Hz
    double if_band_offset = 0.0;             ///< IF band offset in Hz
    double sample_rate = 0.0;                ///< Sample rate in Hz
    uint32_t state_and_event_indicators = 0; ///< State and event indicator field
    uint64_t payload_format = 0;             ///< Data packet payload format field
    size_t packet_size = ALT_CONTEXT_SIZE;   ///< Packet size in bytes (72)
    std::vector<int8_t> raw;                 ///< Raw bytes of the context packet
};

// --- Inline Member Functions ---

inline void difi::swap_endian_16(std::span<uint16_t> data)
{
    swap_endian_16(data.data(), data.size());
}

inline difi::header difi::parse_header(std::span<const uint8_t> buf)
{
    return parse_header(buf.data(), buf.size());
}

inline void difi::pack_header(std::span<uint8_t> buf, const header& header)
{
    pack_header(buf.data(), header);
}

template <difi::context_version V>
inline difi::context<V> difi::parse_context(std::span<const uint8_t> buf)
{
    return parse_context<V>(buf.data(), buf.size());
}

inline difi::context_variant difi::parse_context(std::span<const uint8_t> buf)
{
    return parse_context(buf.data(), buf.size());
}

template <sample_type T>
inline void difi::pack_samples(std::span<const T> in,
                               std::span<uint8_t> payload,
                               int depth,
                               int scaling_mode,
                               float gain,
                               gr_complex offset)
{
    pack_samples(in.data(), in.size(), payload.data(), depth, scaling_mode, gain, offset);
}

template <sample_type T>
inline size_t
difi::unpack_samples(std::span<const uint8_t> payload, std::span<T> out, int depth)
{
    return unpack_samples(payload.data(), payload.size(), out.data(), out.size(), depth);
}

// --- Namespace-Level Aliases for Backwards Compatibility ---
using packet_type = difi::packet_type;
using bit_depth = difi::bit_depth;
using context_version = difi::context_version;
using context_behavior = difi::context_behavior;
using difi_header = difi::header;

template <difi::context_version V = difi::context_version::standard>
using context = difi::context<V>;

using standard_context = difi::standard_context;
using alternative_context = difi::alternative_context;
using difi_context = difi::difi_context;
using context_variant = difi::context_variant;
using difi_handler = difi;

} // namespace gr::difi

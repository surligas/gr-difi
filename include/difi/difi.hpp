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
#include <chrono>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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
concept sample_type = std::same_as<T, gr_complex> ||
                      std::same_as<T, std::complex<char>> ||
                      std::same_as<T, std::complex<int8_t>> ||
                      std::same_as<T, std::complex<int16_t>>;

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
        version = 5, ///< Version Flow Signal Context Packet (DIFI 1.1 / 1.2.1)
        command = 6  ///< Command / Flow Control Packet (VITA 49.2 / DIFI Flow Control)
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
     * @brief Reference point identifiers according to IEEE-ISTO 4900-2021 Section 5.2.
     *
     * Identifies a physical or logical point in the system to which parameters
     * such as Reference Level, frequencies, and Timestamp Adjustment pertain.
     */
    enum class reference_point : uint32_t {
        unspecified = 0,     ///< Unspecified reference point
        antenna_feed = 25,   ///< Antenna feed port (0x0019)
        rf_converter = 75,   ///< RF converter / RFC analog boundary (0x004B)
        if_converter = 100   ///< IF converter / IFC analog boundary (0x0064, default)
    };

    /**
     * @brief Packet class codes for DIFI / VITA 49 (Word 4, bits 15-0 of Class ID).
     *
     * According to IEEE-ISTO 4900-2021 Section 4.3.
     */
    enum class packet_class : uint16_t {
        standard_data = 0x0000,             ///< Standard Flow Signal Data
        standard_context = 0x0001,          ///< Standard Flow Signal Context
        sample_count_data = 0x0002,         ///< Sample Count Signal Data
        sample_count_context = 0x0003,      ///< Sample Count Signal Context
        version_context = 0x0004,           ///< Version Flow Signal Context
        sample_count_flow_control = 0x0005, ///< Sample Count Timing Flow Control
        real_time_flow_control = 0x0006     ///< Real Time TSF Timing Flow Control
    };

    /**
     * @brief Information class codes for DIFI / VITA 49 (Word 4, bits 31-16 of Class ID).
     *
     * According to IEEE-ISTO 4900-2021 Section 4.2.
     */
    enum class information_class : uint16_t {
        basic_data_plane = 0x0000,        ///< Basic Data Plane
        version_flow = 0x0001,            ///< Version Flow
        sample_count_data_plane = 0x0002, ///< Sample Count Data Plane
        bit_padding_data_plane = 0x0003   ///< Bit Padding Permitted Data Plane
    };

    /**
     * @brief Organizationally Unique Identifiers (OUI / CID) for DIFI / VITA 49.
     *
     * Located in Word 3, bits 23-0 of Class ID.
     */
    enum class oui : uint32_t {
        difi = 0x6a621e, ///< IEEE-ISTO / DIFI Consortium Company ID (6A-62-1E)
        vita = 0x7c386c  ///< VITA 49 alternative (7C-38-6C)
    };

    /**
     * @brief Timestamp Integer (TSI) field types according to IEEE-ISTO 4900-2021 Table 7.
     */
    enum class tsi_type : uint8_t {
        none = 0,  ///< No integer-seconds timestamp
        utc = 1,   ///< Coordinated Universal Time (UTC)
        gps = 2,   ///< Global Positioning System (GPS) time
        posix = 3  ///< POSIX time (seconds since 1970-01-01 00:00:00 UTC)
    };

    /**
     * @brief Timestamp Fractional (TSF) field types according to IEEE-ISTO 4900-2021 Table 7.
     */
    enum class tsf_type : uint8_t {
        none = 0,         ///< No fractional-seconds timestamp
        sample_count = 1, ///< Sample count timestamp
        picoseconds = 2,  ///< Real-time timestamp in picoseconds
        free_running = 3  ///< Free-running count timestamp
    };

    /**
     * @brief Context Indicator Field (CIF 0) standard bit masks.
     */
    enum class cif_mask : uint32_t {
        context_change = 0x80000000U,         ///< Context field change indicator (bit 31)
        standard_fields = 0x7bb98000U,        ///< Standard 108-byte context fields (no change bit)
        standard_with_change = 0xfbb98000U,   ///< Standard 108-byte context fields with change bit
        alternative_fields = 0x39a18000U,     ///< Alternative 72-byte context fields (no change bit, decimal 966885376)
        alternative_with_change = 0xb9a18000U ///< Alternative 72-byte context fields with change bit
    };

    /**
     * @brief State and event indicator bit masks (Word 25).
     */
    enum class state_and_event : uint32_t {
        calibrated_time_enable = 0x80000000U,    ///< Calibrated time indicator enable (bit 31)
        reference_lock_enable = 0x20000000U,     ///< Reference frequency lock indicator enable (bit 29)
        calibrated_time_indicator = 0x00800000U, ///< Calibrated time reference locked (bit 19)
        reference_lock_indicator = 0x00020000U,  ///< Reference frequency locked (bit 17)
        default_indicators = 0xa00a0000U         ///< Both time and reference enabled and locked (decimal 2685009920)
    };

    /**
     * @brief Data packet payload format masks (Words 26 & 27).
     */
    enum class payload_format : uint64_t {
        sc8_link_efficient = 0xa00001c700000000ULL,  ///< 8-bit signed Cartesian link-efficient
        sc16_link_efficient = 0xa00003cf00000000ULL  ///< 16-bit signed Cartesian link-efficient
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
        tsi_type tsi = tsi_type::none;  ///< Timestamp Integer type (bits 23-22)
        tsf_type tsf = tsf_type::none;  ///< Timestamp Fractional type (bits 21-20)
    };

    /**
     * @brief Flow control command packet representation (Packet Class 0x0006 / 0x0005).
     *
     * Represents the 84-byte (21 32-bit words) timing flow control packet defined in
     * IEEE-ISTO 4900-2021 Section 4.3 and VITA 49.2, used by hardware to communicate
     * buffer fill level and status back to the stream source.
     */
    struct DIFI_API flow_control {
        packet_type type = packet_type::command; ///< Packet type (6)
        uint8_t pkt_n = 0;              ///< Modulo-16 sequence number
        uint32_t stream_id = 0;         ///< Stream identifier (word 1)
        uint64_t class_id = 0;          ///< Class identifier (words 2-3)
        uint32_t message_id = 0;        ///< Message ID (word 4)
        uint32_t controllee_id = 0;     ///< Controllee ID (word 5)
        uint32_t controller_id = 0;     ///< Controller ID (word 6)
        uint32_t full = 0;              ///< Integer seconds timestamp (word 7)
        uint64_t frac = 0;              ///< Fractional timestamp (words 8-9)
        uint32_t cif0 = 0;              ///< Control Indicator Field 0 (word 11)
        uint32_t cif1 = 0;              ///< Control Indicator Field 1 (word 12)
        reference_point ref_point = reference_point::if_converter; ///< Reference Point (word 13)
        double sample_rate = 0.0;       ///< Sample rate in Hz (words 14-15)
        int64_t timestamp_adjustment = 0; ///< Timestamp adjustment in femtoseconds (words 16-17)
        uint64_t buffer_size = 0;       ///< Stream sink buffer size in bytes (words 18-19)
        uint16_t buffer_level = 0;      ///< 12-bit buffer level 0..4095 (word 20 bits 15-4)
        bool overflow = false;          ///< Buffer overflow flag (word 20 bit 3)
        bool nearly_full = false;       ///< Nearly full flag (word 20 bit 2)
        bool nearly_empty = false;      ///< Nearly empty flag (word 20 bit 1)
        bool underflow = false;         ///< Buffer underflow flag (word 20 bit 0)
        tsi_type tsi = tsi_type::none;  ///< Timestamp Integer type (bits 23-22)
        tsf_type tsf = tsf_type::none;  ///< Timestamp Fractional type (bits 21-20)

        /// Returns buffer fill level as a percentage (0.0% to 100.0%).
        float buffer_fill_pct() const {
            return (static_cast<float>(buffer_level) / 4095.0f) * 100.0f;
        }
    };

    // --- Wire Format Constants ---
    static constexpr size_t HEADER_SIZE = 28;
    static constexpr size_t STANDARD_CONTEXT_SIZE = 108;
    static constexpr size_t ALT_CONTEXT_SIZE = 72;
    static constexpr size_t FLOW_CONTROL_SIZE = 84;
    static constexpr size_t FLOW_CONTROL_WORDS = 21;
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
    static constexpr uint64_t DEFAULT_STATE_AND_EVENTS =
        static_cast<uint64_t>(state_and_event::default_indicators);
    static constexpr uint64_t EIGHT_BIT_SIGNED_CART_LINK_EFF =
        static_cast<uint64_t>(payload_format::sc8_link_efficient);
    static constexpr uint64_t SIXTEEN_BIT_SIGNED_CART_LINK_EFF =
        static_cast<uint64_t>(payload_format::sc16_link_efficient);
    static constexpr uint64_t STANDARD_OUI = static_cast<uint64_t>(oui::difi);
    static constexpr uint64_t ALT_OUI = static_cast<uint64_t>(oui::vita);
    static constexpr size_t STANDARD_CONTEXT_PACKET_SIZE = STANDARD_CONTEXT_SIZE;
    static constexpr size_t ALT_CONTEXT_PACKET_SIZE = ALT_CONTEXT_SIZE;
    static constexpr uint32_t STANDARD_DATA_STATIC_BITS = 0x18e00000;
    static constexpr uint32_t STANDARD_CONTEXT_STATIC_BITS = 0x49e00000;
    static constexpr uint32_t ALT_CONTEXT_STATIC_BITS = 0x49000000;

    /**
     * @brief Constructs a 64-bit VITA 49 / DIFI Class ID from OUI, packet class, and information class.
     *
     * @param id_oui Organizationally Unique Identifier (default: oui::difi)
     * @param pkt_cls Packet class code (default: packet_class::standard_data)
     * @param info_cls Information class code (default: information_class::basic_data_plane)
     * @param pad_bits Number of pad bits in final word of payload (0..31, default: 0)
     * @return 64-bit Class ID word
     */
    static constexpr uint64_t make_class_id(
        oui id_oui = oui::difi,
        packet_class pkt_cls = packet_class::standard_data,
        information_class info_cls = information_class::basic_data_plane,
        uint8_t pad_bits = 0)
    {
        uint64_t w3 = ((static_cast<uint64_t>(pad_bits) & 0x1fULL) << 27) |
                      (static_cast<uint64_t>(id_oui) & 0x00ffffffULL);
        uint64_t w4 = (static_cast<uint64_t>(info_cls) << 16) |
                      (static_cast<uint64_t>(pkt_cls) & 0xffffULL);
        return (w3 << 32) | w4;
    }

    /**
     * @brief Generates header static bits (bits 31-20) for a data packet.
     *
     * @param tsi Timestamp Integer type (default: tsi_type::posix)
     * @param tsf Timestamp Fractional type (default: tsf_type::picoseconds)
     * @return uint32_t static bits with packet type 1 (Data) and Class ID present
     */
    static constexpr uint32_t make_data_static_bits(
        tsi_type tsi = tsi_type::posix,
        tsf_type tsf = tsf_type::picoseconds)
    {
        return 0x18000000U |
               ((static_cast<uint32_t>(tsi) & 0x3U) << 22) |
               ((static_cast<uint32_t>(tsf) & 0x3U) << 20);
    }

    /**
     * @brief Generates header static bits (bits 31-20) for a standard context packet.
     *
     * @param tsi Timestamp Integer type (default: tsi_type::posix)
     * @param tsf Timestamp Fractional type (default: tsf_type::picoseconds)
     * @return uint32_t static bits with packet type 4 (Context) and Class ID present
     */
    static constexpr uint32_t make_context_static_bits(
        tsi_type tsi = tsi_type::posix,
        tsf_type tsf = tsf_type::picoseconds)
    {
        return 0x49000000U |
               ((static_cast<uint32_t>(tsi) & 0x3U) << 22) |
               ((static_cast<uint32_t>(tsf) & 0x3U) << 20);
    }

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

    /**
     * @brief Configuration parameters for creating a context packet.
     */
    struct context_config {
        double sample_rate = 1.0;                ///< Sample rate in Hz
        int depth = 16;                          ///< Bit depth (8 or 16)
        uint32_t stream_id = 0;                  ///< Stream identifier
        double rf_freq = 0.0;                    ///< RF reference frequency in Hz (default: 0.0)
        double if_freq = 0.0;                    ///< IF reference frequency in Hz (default: 0.0)
        double bandwidth = 0.0;                  ///< Bandwidth in Hz (0.0 defaults to sample_rate)
        double if_band_offset = 0.0;             ///< IF band offset in Hz (default: 0.0)
        float rf_gain = 0.0f;                    ///< RF gain in dB (standard_context only, default: 0.0)
        float if_gain = 0.0f;                    ///< IF gain in dB (standard_context only, default: 0.0)
        float ref_level = 0.0f;                  ///< Reference level in dBm (standard_context only, default: 0.0)
        uint32_t full = 0;                       ///< Integer seconds timestamp (default: 0)
        uint64_t frac = 0;                       ///< Fractional picoseconds timestamp (default: 0)
        int64_t timestamp_adjustment = 0;        ///< Timestamp adjustment in femtoseconds (default: 0)
        uint32_t timestamp_calibration_time = 0; ///< Timestamp calibration time in seconds (0 defaults to full)
        uint64_t class_id = 0;                   ///< Class identifier (0 defaults to standard/alternative OUI)
        uint32_t cif = 0;                        ///< Context indicator field (0 defaults to standard/alternative default)
        reference_point ref_point =
            reference_point::if_converter;       ///< Reference point (default: if_converter = 100 / 0x64)
        uint32_t state_and_event_indicators = 0; ///< State and event indicators (0 defaults to DEFAULT_STATE_AND_EVENTS)
        tsi_type tsi = tsi_type::posix;          ///< Timestamp Integer time source (default: posix)
    };

    // --- Byte-Order Serialization Helpers ---

    /**
     * @brief Serializes a 32-bit unsigned integer in network byte order (big-endian).
     *
     * @param ptr destination buffer (must have at least 4 bytes)
     * @param val 32-bit value to serialize
     */
    static inline void pack_u32(void* ptr, uint32_t val)
    {
        uint32_t net = htonl(val);
        std::memcpy(ptr, &net, sizeof(net));
    }

    /**
     * @brief Serializes a 64-bit unsigned integer in network byte order (big-endian).
     *
     * @param ptr destination buffer (must have at least 8 bytes)
     * @param val 64-bit value to serialize
     */
    static inline void pack_u64(void* ptr, uint64_t val)
    {
        uint64_t net = htobe64(val);
        std::memcpy(ptr, &net, sizeof(net));
    }

    /**
     * @brief Deserializes a 32-bit unsigned integer from network byte order (big-endian).
     *
     * @param ptr source buffer (must have at least 4 bytes)
     * @return uint32_t host-order value
     */
    static inline uint32_t unpack_u32(const void* ptr)
    {
        uint32_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return ntohl(val);
    }

    /**
     * @brief Deserializes a 64-bit unsigned integer from network byte order (big-endian).
     *
     * @param ptr source buffer (must have at least 8 bytes)
     * @return uint64_t host-order value
     */
    static inline uint64_t unpack_u64(const void* ptr)
    {
        uint64_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return be64toh(val);
    }

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
     * @brief Serializes a typed context packet into a destination byte span.
     *
     * @tparam V context version
     * @param ctx context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param out destination byte span (must have at least context packet size bytes)
     */
    template <context_version V>
    static void pack_context(const context<V>& ctx,
                             uint8_t pkt_n,
                             uint32_t stream_id,
                             std::span<uint8_t> out);

    /**
     * @brief Serializes a standard context packet into a destination byte span.
     *
     * @param ctx standard context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param out destination byte span (must have at least STANDARD_CONTEXT_SIZE bytes)
     */
    static void pack_context(const standard_context& ctx,
                             uint8_t pkt_n,
                             uint32_t stream_id,
                             std::span<uint8_t> out);

    /**
     * @brief Serializes an alternative context packet into a destination byte span.
     *
     * @param ctx alternative context fields in engineering units
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param out destination byte span (must have at least ALT_CONTEXT_SIZE bytes)
     */
    static void pack_context(const alternative_context& ctx,
                             uint8_t pkt_n,
                             uint32_t stream_id,
                             std::span<uint8_t> out);

    /**
     * @brief Serializes a context packet variant into a destination byte span.
     *
     * @param ctx context variant (standard or alternative)
     * @param pkt_n packet sequence number (0..15)
     * @param stream_id stream identifier
     * @param out destination byte span
     */
    static void pack_context(const context_variant& ctx,
                             uint8_t pkt_n,
                             uint32_t stream_id,
                             std::span<uint8_t> out);

    /**
     * @brief Creates a typed context packet from a configuration struct.
     *
     * @tparam V context version (standard or alternative)
     * @param cfg context configuration
     * @return context<V> configured context packet
     */
    template <context_version V = context_version::standard>
    static context<V> make_context(const context_config& cfg);

    /**
     * @brief Creates a context packet variant based on the requested version.
     *
     * @param version context version (standard or alternative)
     * @param cfg context configuration
     * @return context_variant containing standard_context or alternative_context
     */
    static context_variant make_context(context_version version, const context_config& cfg);

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

    // --- Flow Control Operations ---

    /**
     * @brief Parses an 84-byte VITA 49.2 / DIFI Flow Control packet (Packet Class 0x0006 / 0x0005).
     *
     * @param buf pointer to packet buffer
     * @param len buffer length in bytes (must be at least 84)
     * @return std::optional<flow_control> parsed flow control struct, or std::nullopt if invalid
     */
    static std::optional<flow_control> parse_flow_control(const void* buf, size_t len);

    /**
     * @brief Parses a VITA 49.2 / DIFI Flow Control packet from a byte span.
     */
    static std::optional<flow_control> parse_flow_control(std::span<const uint8_t> span);

    /**
     * @brief Serializes a flow_control struct into an 84-byte destination byte span.
     *
     * @param fc flow control struct
     * @param out destination byte span (must have at least 84 bytes)
     */
    static void pack_flow_control(const flow_control& fc, std::span<uint8_t> out);

    /**
     * @brief Serializes a flow_control struct into a newly allocated vector.
     */
    static std::vector<uint8_t> pack_flow_control(const flow_control& fc);

    // --- Timestamp Arithmetic ---

    /**
     * @brief Generates a DIFI timestamp from the host wall-clock for a given time source.
     *
     * Supports POSIX (std::chrono::system_clock, no leap seconds), UTC (std::chrono::utc_clock,
     * includes leap seconds), and GPS (std::chrono::gps_clock, epoch Jan 6 1980).
     *
     * @param lead_time_us optional lead time in microseconds to add into the future
     * @param tsi Timestamp Integer time source (default: tsi_type::posix)
     * @return std::pair<uint32_t, uint64_t> {full, frac} where full is seconds
     *         and frac is fractional picoseconds (0..999,999,999,999).
     */
    static std::pair<uint32_t, uint64_t> system_timestamp(uint32_t lead_time_us = 0,
                                                          tsi_type tsi = tsi_type::posix);

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
     * @brief Packs complex samples into raw payload bytes without scaling.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input complex samples
     * @param num_samples number of complex samples
     * @param payload output buffer
     * @param depth bit depth (8 or 16)
     */
    template <sample_type T>
    static void pack_samples(const T* in,
                             size_t num_samples,
                             void* payload,
                             int depth);

    /**
     * @brief Packs complex samples into raw payload span without scaling.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input sample span
     * @param payload output payload span
     * @param depth bit depth (8 or 16)
     */
    template <sample_type T>
    static void pack_samples(std::span<const T> in,
                             std::span<uint8_t> payload,
                             int depth);

    /**
     * @brief Packs complex samples into raw payload bytes with gain and offset.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input complex samples
     * @param num_samples number of complex samples
     * @param payload output buffer
     * @param depth bit depth (8 or 16)
     * @param gain scalar multiplier
     * @param offset complex offset added prior to gain
     */
    template <sample_type T>
    static void pack_samples(const T* in,
                             size_t num_samples,
                             void* payload,
                             int depth,
                             float gain,
                             gr_complex offset = { 0.0f, 0.0f });

    /**
     * @brief Packs complex samples into raw payload span with gain and offset.
     *
     * @tparam T sample type (gr_complex or std::complex<char>)
     * @param in input sample span
     * @param payload output payload span
     * @param depth bit depth (8 or 16)
     * @param gain scalar multiplier
     * @param offset complex offset added prior to gain
     */
    template <sample_type T>
    static void pack_samples(std::span<const T> in,
                             std::span<uint8_t> payload,
                             int depth,
                             float gain,
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
     * @brief Converts a flow_control struct to a GNU Radio PMT dictionary.
     *
     * Contains buffer_fill_pct, buffer_level, buffer_size, overflow,
     * nearly_full, nearly_empty, underflow, stream_id, pkt_n, full, frac,
     * sample_rate, and timestamp_adjustment.
     *
     * @param fc flow_control struct
     * @return PMT dictionary
     */
    static pmt::pmt_t flow_control_to_pmt(const flow_control& fc);

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
    uint32_t cif = 0;                                           ///< Context Indicator Field
    reference_point ref_point = reference_point::if_converter;  ///< Reference point
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
    tsi_type tsi = tsi_type::posix;             ///< Timestamp Integer time source
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
                               int depth)
{
    pack_samples(in.data(), in.size(), payload.data(), depth);
}

template <sample_type T>
inline void difi::pack_samples(std::span<const T> in,
                               std::span<uint8_t> payload,
                               int depth,
                               float gain,
                               gr_complex offset)
{
    pack_samples(in.data(), in.size(), payload.data(), depth, gain, offset);
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
using reference_point = difi::reference_point;
using packet_class = difi::packet_class;
using information_class = difi::information_class;
using oui = difi::oui;
using tsi_type = difi::tsi_type;
using tsf_type = difi::tsf_type;
using cif_mask = difi::cif_mask;
using state_and_event = difi::state_and_event;
using payload_format = difi::payload_format;
using difi_header = difi::header;

template <difi::context_version V = difi::context_version::standard>
using context = difi::context<V>;

using standard_context = difi::standard_context;
using alternative_context = difi::alternative_context;
using difi_context = difi::difi_context;
using context_variant = difi::context_variant;
using flow_control = difi::flow_control;
using difi_flow_control = difi::flow_control;
using difi_handler = difi;

inline void pack_u32(void* ptr, uint32_t val)
{
    difi::pack_u32(ptr, val);
}

inline void pack_u64(void* ptr, uint64_t val)
{
    difi::pack_u64(ptr, val);
}

inline uint32_t unpack_u32(const void* ptr)
{
    return difi::unpack_u32(ptr);
}

inline uint64_t unpack_u64(const void* ptr)
{
    return difi::unpack_u64(ptr);
}

} // namespace gr::difi

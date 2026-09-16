/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <difi/api.h>
#include <gnuradio/types.h>
#include <pmt/pmt.h>

#include <bit>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string>
#include <utility>
#include <vector>

namespace gr::difi {

// --- Wire Format Constants ---
inline constexpr uint8_t DIFI_HEADER_SIZE = 28;
inline constexpr uint8_t VITA_PKT_MOD = 16;
inline constexpr uint32_t MS_DATA_VITA_HEADER = 0x18;
inline constexpr uint64_t PICO_CONVERSION = 1000000000000ULL;
inline constexpr uint16_t CONTEXT_PACKET_OFFSETS[16] = {
    8, 16, 20, 28, 32, 36, 44, 52, 60, 68, 72, 76, 84, 92, 96, 100};
inline constexpr uint16_t CONTEXT_PACKET_ALT_OFFSETS[9] = {
    8, 16, 20, 28, 36, 44, 52, 60, 64};
inline constexpr uint64_t EIGHT_BIT_SIGNED_CART_LINK_EFF =
    0xa00003c700000000ULL;
inline constexpr uint64_t SIXTEEN_BIT_SIGNED_CART_LINK_EFF =
    0xa00007cf00000000ULL;
inline constexpr uint32_t DATA_START_IDX = 28;
inline constexpr uint64_t DEFAULT_STATE_AND_EVENTS = 2685009920ULL;
inline constexpr uint64_t STANDARD_OUI = 0x6a621e;
inline constexpr uint64_t ALT_OUI = 0x7c386c;
inline constexpr size_t STANDARD_CONTEXT_PACKET_SIZE = 108;
inline constexpr size_t ALT_CONTEXT_PACKET_SIZE = 72;
inline constexpr uint32_t STANDARD_DATA_STATIC_BITS = 0x18e00000;
inline constexpr uint32_t STANDARD_CONTEXT_STATIC_BITS = 0x49e00000;
inline constexpr uint32_t ALT_CONTEXT_STATIC_BITS = 0x49000000;

/**
 * @brief DIFI packet types per IEEE-ISTO 4900-2021 and VITA 49.2.
 */
enum class packet_type : uint8_t {
  unknown = 0,
  data = 1,    ///< Standard Flow Signal Data Packet
  context = 4  ///< Standard Flow Signal Context Packet
};

/**
 * @brief Bit depths supported by DIFI.
 */
enum class bit_depth : uint8_t {
  bits_8 = 8,
  bits_16 = 16
};

/**
 * @brief Context packet handling behavior when an unexpected context packet arrives.
 */
enum class context_behavior : uint8_t {
  throw_exe = 0,
  ignore = 1,
  warnings_forward = 2,
  warnings_no_forward = 3
};

/**
 * @brief Decoded fields from a DIFI packet header.
 */
struct DIFI_API difi_header {
  packet_type type = packet_type::unknown;
  uint8_t pkt_n = 0;              ///< Modulo-16 packet sequence number (bits 19-16)
  uint16_t packet_size_words = 0; ///< Packet size in 32-bit words (bits 15-0)
  size_t packet_size_bytes = 0;   ///< Packet size in bytes
  uint32_t stream_id = 0;         ///< Stream identifier (word 1)
  uint64_t class_id = 0;          ///< Class identifier (words 2-3)
  uint32_t full = 0;              ///< Integer seconds (word 4)
  uint64_t frac = 0;              ///< Fractional picoseconds (words 5-6)
  uint32_t static_bits = 0;       ///< Header bits 31-20
  uint32_t raw_header = 0;        ///< Raw header word 0
};

/**
 * @brief Fields of a DIFI context packet in standard engineering units.
 */
struct DIFI_API difi_context {
  uint64_t class_id = 0;
  uint32_t full = 0;                      ///< Integer seconds
  uint64_t frac = 0;                      ///< Fractional picoseconds
  uint32_t cif = 0;                       ///< Context Indicator Field
  uint32_t ref_point = 0;                 ///< Reference point
  double bandwidth = 0.0;                 ///< Bandwidth in Hz
  double if_ref_freq = 0.0;               ///< IF reference frequency in Hz
  double rf_ref_freq = 0.0;               ///< RF reference frequency in Hz
  double if_band_offset = 0.0;            ///< IF band offset in Hz
  float ref_level = 0.0f;                 ///< Reference level in dBm
  float rf_gain = 0.0f;                   ///< RF gain in dB
  float if_gain = 0.0f;                   ///< IF gain in dB
  double sample_rate = 0.0;               ///< Sample rate in Hz
  int64_t timestamp_adjustment = 0;       ///< Timestamp adjustment in femtoseconds
  uint32_t timestamp_calibration_time = 0;///< Timestamp calibration time in seconds
  uint32_t state_and_event_indicators = 0;///< State and event indicator field
  uint64_t payload_format = 0;            ///< Data packet payload format field
  size_t packet_size = 108;               ///< Packet size in bytes (108 standard, 72 alt)
  std::vector<int8_t> raw;                ///< Raw bytes of the context packet
};

/**
 * @brief Central class for DIFI and VITA 49 protocol tasks.
 *
 * Provides header parsing and serialization, context packet construction and
 * decoding, VITA-49 fixed-point math, sample packing and unpacking with SIMD
 * endianness conversion via VOLK, timestamp accumulation, and PMT dictionary
 * conversions for GNU Radio stream tags.
 */
class DIFI_API difi {
public:
  static constexpr size_t HEADER_SIZE = 28;
  static constexpr size_t STANDARD_CONTEXT_SIZE = 108;
  static constexpr size_t ALT_CONTEXT_SIZE = 72;
  static constexpr uint8_t PACKET_MOD = 16;
  static constexpr uint64_t PICO_PER_SEC = 1000000000000ULL;

  // Constants mirrored for convenience
  static constexpr uint8_t DIFI_HEADER_SIZE = 28;
  static constexpr uint8_t VITA_PKT_MOD = 16;
  static constexpr uint32_t MS_DATA_VITA_HEADER = 0x18;
  static constexpr uint64_t PICO_CONVERSION = 1000000000000ULL;
  static constexpr uint16_t CONTEXT_PACKET_OFFSETS[16] = {
      8, 16, 20, 28, 32, 36, 44, 52, 60, 68, 72, 76, 84, 92, 96, 100};
  static constexpr uint16_t CONTEXT_PACKET_ALT_OFFSETS[9] = {
      8, 16, 20, 28, 36, 44, 52, 60, 64};
  static constexpr uint32_t DATA_START_IDX = 28;
  static constexpr uint64_t DEFAULT_STATE_AND_EVENTS = 2685009920ULL;
  static constexpr uint64_t EIGHT_BIT_SIGNED_CART_LINK_EFF = 0xa00003c700000000ULL;
  static constexpr uint64_t SIXTEEN_BIT_SIGNED_CART_LINK_EFF = 0xa00007cf00000000ULL;
  static constexpr uint64_t STANDARD_OUI = 0x6a621e;
  static constexpr uint64_t ALT_OUI = 0x7c386c;
  static constexpr uint32_t STANDARD_DATA_STATIC_BITS = 0x18e00000;
  static constexpr uint32_t STANDARD_CONTEXT_STATIC_BITS = 0x49e00000;
  static constexpr uint32_t ALT_CONTEXT_STATIC_BITS = 0x49000000;

  // --- Fixed-Point Conversions ---
  static double vita_fixed_to_double(uint64_t bits);
  static double vita_fixed_to_double(int64_t bits);
  static uint64_t double_to_vita_fixed(double val);
  static float vita_fixed_to_float(int16_t bits);
  static int16_t float_to_vita_fixed(float val);

  // --- SIMD Endianness Conversion ---
  /**
   * @brief Byte-swaps a vector of 16-bit unsigned integers using VOLK SIMD kernels.
   *
   * On little-endian architectures, calls volk_16u_byteswap. On big-endian
   * architectures, this is a compile-time no-op.
   *
   * @param data pointer to 16-bit elements
   * @param num_points number of 16-bit elements to swap
   */
  static void swap_endian_16(uint16_t *data, size_t num_points);

  // --- Header Operations ---
  /**
   * @brief Parses a 28-byte DIFI header from a buffer.
   *
   * @param buf pointer to buffer
   * @param len size of buffer in bytes
   * @return decoded difi_header
   */
  static difi_header parse_header(const void *buf, size_t len);

  /**
   * @brief Serializes a 28-byte DIFI header into a buffer.
   *
   * @param buf pointer to output buffer (must have at least HEADER_SIZE bytes)
   * @param header header details to pack
   */
  static void pack_header(void *buf, const difi_header &header);

  // --- Context Packet Operations ---
  /**
   * @brief Parses a context packet (108-byte standard or 72-byte alternative).
   *
   * @param buf pointer to packet buffer
   * @param len size of buffer in bytes
   * @return decoded difi_context
   */
  static difi_context parse_context(const void *buf, size_t len);

  /**
   * @brief Creates a context packet from structured fields.
   *
   * @param ctx context fields in engineering units
   * @param pkt_n packet sequence number (0..15)
   * @param stream_id stream identifier
   * @param packet_size packet size in bytes (108 standard or 72 alt)
   * @return byte vector containing the packed context packet
   */
  static std::vector<uint8_t> pack_context(const difi_context &ctx,
                                           uint8_t pkt_n,
                                           uint32_t stream_id,
                                           size_t packet_size = STANDARD_CONTEXT_SIZE);

  /**
   * @brief Creates a default context packet for standalone sink mode.
   *
   * @param samp_rate sample rate in Hz
   * @param depth bit depth (8 or 16)
   * @param stream_id stream identifier
   * @param full reference integer timestamp
   * @param frac reference fractional timestamp
   * @param packet_size 108 or 72
   * @return difi_context configured with standard DIFI default values
   */
  static difi_context make_default_context(double samp_rate,
                                           int depth,
                                           uint32_t stream_id,
                                           uint32_t full = 0,
                                           uint64_t frac = 0,
                                           size_t packet_size = STANDARD_CONTEXT_SIZE);

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
  static std::pair<uint32_t, uint64_t> advance_timestamp(uint32_t full,
                                                         uint64_t frac,
                                                         size_t num_samples,
                                                         double samp_rate);

  // --- Sample Packing and Unpacking ---
  /**
   * @brief Packs complex samples into raw payload bytes with optional scaling.
   *
   * For 16-bit depth, converts samples to 16-bit integers and applies VOLK
   * SIMD byte-swapping to convert to network big-endian order.
   * For 8-bit depth, converts samples to 8-bit signed characters without byte swapping.
   *
   * @tparam T sample type (gr_complex or std::complex<char>)
   * @param in input complex samples
   * @param num_samples number of complex samples
   * @param payload output buffer (must hold num_samples * 2 * (depth / 8) bytes)
   * @param depth bit depth (8 or 16)
   * @param scaling_mode 0: none, 1: manual (gain & offset)
   * @param gain scalar multiplier for manual scaling
   * @param offset complex offset added prior to gain
   */
  template <typename T>
  static void pack_samples(const T *in,
                           size_t num_samples,
                           void *payload,
                           int depth,
                           int scaling_mode = 0,
                           float gain = 1.0f,
                           gr_complex offset = {0.0f, 0.0f});

  /**
   * @brief Unpacks complex samples from raw payload bytes.
   *
   * For 16-bit depth, uses VOLK SIMD byte-swapping to convert network big-endian
   * 16-bit words to host order, then produces complex samples.
   * For 8-bit depth, reads signed 8-bit integers directly.
   *
   * @tparam T sample type (gr_complex or std::complex<char>)
   * @param payload input payload buffer
   * @param payload_bytes number of payload bytes available
   * @param out output sample buffer
   * @param max_samples maximum number of samples out can hold
   * @param depth bit depth (8 or 16)
   * @return number of complex samples unpacked
   */
  template <typename T>
  static size_t unpack_samples(const void *payload,
                               size_t payload_bytes,
                               T *out,
                               size_t max_samples,
                               int depth);

  // --- GNU Radio PMT Interoperability ---
  static pmt::pmt_t context_to_pmt(const difi_header &header, const difi_context &ctx);
  static difi_context pmt_to_context(pmt::pmt_t dict);
  static pmt::pmt_t make_pkt_n_dict(uint8_t pkt_n, size_t data_len, uint32_t full, uint64_t frac);
};

using difi_handler = difi;

} // namespace gr::difi

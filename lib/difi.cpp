/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include <difi/difi.hpp>

#include <volk/volk.h>
#include <volk/volk_16u_byteswap.h>

#include <arpa/inet.h>
#include <chrono>
#include <endian.h>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gr::difi {

double difi::vita_fixed_to_double(uint64_t bits)
{
    return static_cast<double>(bits) / static_cast<double>(1ULL << 20);
}

double difi::vita_fixed_to_double(int64_t bits)
{
    return static_cast<double>(bits) / static_cast<double>(1ULL << 20);
}

uint64_t difi::double_to_vita_fixed(double val)
{
    return static_cast<uint64_t>(val * static_cast<double>(1ULL << 20));
}

float difi::vita_fixed_to_float(int16_t bits)
{
    return static_cast<float>(bits) / static_cast<float>(1 << 7);
}

int16_t difi::float_to_vita_fixed(float val)
{
    return static_cast<int16_t>(std::round(val * static_cast<float>(1 << 7)));
}

void difi::swap_endian_16(uint16_t* data, size_t num_points)
{
    if constexpr (std::endian::native == std::endian::little) {
        if (num_points > 0 && data != nullptr) {
            volk_16u_byteswap(data, static_cast<unsigned int>(num_points));
        }
    }
}

difi::header difi::parse_header(const void* buf, size_t len)
{
    if (len < HEADER_SIZE) {
        throw std::runtime_error("Buffer length " + std::to_string(len) +
                                 " is smaller than DIFI header size (28 bytes)");
    }

    const uint8_t* in = static_cast<const uint8_t*>(buf);

    uint32_t w0 = unpack_u32(in);
    uint32_t w1 = unpack_u32(in + 4);
    uint64_t cid = unpack_u64(in + 8);
    uint32_t full = unpack_u32(in + 16);
    uint64_t frac = unpack_u64(in + 20);

    header h;
    uint8_t type_val = static_cast<uint8_t>(w0 >> 28);
    if (type_val == static_cast<uint8_t>(packet_type::data)) {
        h.type = packet_type::data;
    } else if (type_val == static_cast<uint8_t>(packet_type::context)) {
        // In DIFI v1.2.1, version flow packets use context pktType 0x4 with
        // packetClassCode 0x0004
        if ((cid & 0xffff) == static_cast<uint16_t>(packet_class::version_context)) {
            h.type = packet_type::version;
        } else {
            h.type = packet_type::context;
        }
    } else if (type_val == static_cast<uint8_t>(packet_type::version)) {
        h.type = packet_type::version;
    } else if (type_val == static_cast<uint8_t>(packet_type::command)) {
        h.type = packet_type::command;
    } else {
        h.type = packet_type::unknown;
    }

    h.pkt_n = static_cast<uint8_t>((w0 >> 16) & 0x0f);
    h.packet_size_words = static_cast<uint16_t>(w0 & 0xffff);
    h.packet_size_bytes = static_cast<size_t>(h.packet_size_words) * 4;
    h.stream_id = w1;
    h.class_id = cid;
    h.full = full;
    h.frac = frac;
    h.static_bits = w0 & 0xfff00000;
    h.raw_header = w0;
    h.tsi = static_cast<tsi_type>((w0 >> 22) & 0x03);
    h.tsf = static_cast<tsf_type>((w0 >> 20) & 0x03);

    return h;
}

void difi::pack_header(void* buf, const header& header)
{
    uint8_t* out = static_cast<uint8_t*>(buf);

    uint32_t w0 = header.raw_header;
    if (w0 == 0) {
        uint32_t static_part = header.static_bits;
        if (static_part == 0) {
            if (header.type == packet_type::context) {
                static_part = make_context_static_bits(
                    header.tsi != tsi_type::none ? header.tsi : tsi_type::posix,
                    header.tsf != tsf_type::none ? header.tsf : tsf_type::picoseconds);
            } else if (header.type == packet_type::version) {
                static_part = (static_cast<uint32_t>(packet_type::version) << 28) |
                              (STANDARD_CONTEXT_STATIC_BITS & 0x0fffffff);
            } else {
                static_part = make_data_static_bits(
                    header.tsi != tsi_type::none ? header.tsi : tsi_type::posix,
                    header.tsf != tsf_type::none ? header.tsf : tsf_type::picoseconds);
            }
        } else if (header.type != packet_type::unknown) {
            static_part =
                (static_cast<uint32_t>(header.type) << 28) | (static_part & 0x0fffffff);
        }
        w0 = static_part | (static_cast<uint32_t>(header.pkt_n & 0x0f) << 16) |
             (static_cast<uint32_t>(header.packet_size_words) & 0xffff);
    }

    pack_u32(out, w0);
    pack_u32(out + 4, header.stream_id);
    pack_u64(out + 8, header.class_id);
    pack_u32(out + 16, header.full);
    pack_u64(out + 20, header.frac);
}

template <>
difi::standard_context
difi::parse_context<difi::context_version::standard>(const void* buf, size_t len)
{
    if (len < STANDARD_CONTEXT_SIZE) {
        throw std::runtime_error("Context packet length " + std::to_string(len) +
                                 " is smaller than standard context size (108 bytes)");
    }

    const uint8_t* in = static_cast<const uint8_t*>(buf);
    standard_context ctx;
    ctx.packet_size = len;
    ctx.raw.resize(len);
    std::memcpy(ctx.raw.data(), in, len);

    uint32_t w0 = unpack_u32(in);
    ctx.tsi = static_cast<tsi_type>((w0 >> 22) & 0x03);

    int idx = 0;
    ctx.class_id = unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.full = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.frac = unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.cif = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.ref_point = static_cast<reference_point>(unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.bandwidth = vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.if_ref_freq =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.rf_ref_freq =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.if_band_offset = vita_fixed_to_double(
        static_cast<int64_t>(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]])));
    ctx.ref_level = vita_fixed_to_float(
        static_cast<int16_t>(0xffffU & unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]])));
    uint32_t gains = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.rf_gain = vita_fixed_to_float(static_cast<int16_t>(0xffffU & gains));
    ctx.if_gain = vita_fixed_to_float(static_cast<int16_t>(gains >> 16));
    ctx.sample_rate =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.timestamp_adjustment =
        static_cast<int64_t>(unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]));
    ctx.timestamp_calibration_time = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.state_and_event_indicators = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.payload_format = unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]);

    return ctx;
}

template <>
difi::alternative_context
difi::parse_context<difi::context_version::alternative>(const void* buf, size_t len)
{
    if (len < ALT_CONTEXT_SIZE) {
        throw std::runtime_error("Context packet length " + std::to_string(len) +
                                 " is smaller than alternative context size (72 bytes)");
    }

    const uint8_t* in = static_cast<const uint8_t*>(buf);
    alternative_context ctx;
    ctx.packet_size = len;
    ctx.raw.resize(len);
    std::memcpy(ctx.raw.data(), in, len);

    int idx = 0;
    ctx.class_id = unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]);
    ctx.cif = unpack_u32(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]);
    ctx.bandwidth =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]));
    ctx.if_ref_freq =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]));
    ctx.rf_ref_freq =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]));
    ctx.if_band_offset = vita_fixed_to_double(
        static_cast<int64_t>(unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]])));
    ctx.sample_rate =
        vita_fixed_to_double(unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]));
    ctx.state_and_event_indicators = unpack_u32(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]);
    ctx.payload_format = unpack_u64(&in[CONTEXT_PACKET_ALT_OFFSETS[idx++]]);

    return ctx;
}

difi::context_variant difi::parse_context(const void* buf, size_t len)
{
    if (len == ALT_CONTEXT_SIZE) {
        return parse_context<context_version::alternative>(buf, len);
    }
    if (len >= STANDARD_CONTEXT_SIZE) {
        return parse_context<context_version::standard>(buf, len);
    }
    throw std::runtime_error("Invalid context packet length: " + std::to_string(len) +
                             " bytes (expected 72 or 108 bytes)");
}

template <>
void difi::pack_context<difi::context_version::standard>(
    const standard_context& ctx, uint8_t pkt_n, uint32_t stream_id, std::span<uint8_t> out)
{
    if (out.size() < STANDARD_CONTEXT_SIZE) {
        throw std::invalid_argument(
            "Destination span size (" + std::to_string(out.size()) +
            ") is smaller than standard context size (" +
            std::to_string(STANDARD_CONTEXT_SIZE) + ")");
    }

    std::fill(out.begin(), out.end(), 0);

    uint32_t header = make_context_static_bits(ctx.tsi) |
                      (static_cast<uint32_t>(pkt_n & 0x0f) << 16) |
                      static_cast<uint32_t>(out.size() / 4);

    pack_u32(&out[0], header);
    pack_u32(&out[4], stream_id);

    int idx = 0;
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.class_id);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.full);
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.frac);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             ctx.cif != 0 ? ctx.cif : static_cast<uint32_t>(cif_mask::standard_with_change));
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             ctx.ref_point != reference_point::unspecified
                 ? static_cast<uint32_t>(ctx.ref_point)
                 : static_cast<uint32_t>(reference_point::if_converter));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], double_to_vita_fixed(ctx.bandwidth));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], double_to_vita_fixed(ctx.if_ref_freq));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], double_to_vita_fixed(ctx.rf_ref_freq));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.if_band_offset));
    pack_u32(
        &out[CONTEXT_PACKET_OFFSETS[idx++]],
        static_cast<uint32_t>(static_cast<uint16_t>(float_to_vita_fixed(ctx.ref_level))));
    uint32_t gain_word =
        (static_cast<uint32_t>(static_cast<uint16_t>(float_to_vita_fixed(ctx.if_gain)))
         << 16) |
        (static_cast<uint32_t>(static_cast<uint16_t>(float_to_vita_fixed(ctx.rf_gain))));
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]], gain_word);
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], double_to_vita_fixed(ctx.sample_rate));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             static_cast<uint64_t>(ctx.timestamp_adjustment));
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             ctx.timestamp_calibration_time != 0 ? ctx.timestamp_calibration_time
                                                 : ctx.full);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             ctx.state_and_event_indicators != 0
                 ? ctx.state_and_event_indicators
                 : static_cast<uint32_t>(state_and_event::default_indicators));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.payload_format);
}

template <>
void difi::pack_context<difi::context_version::alternative>(
    const alternative_context& ctx, uint8_t pkt_n, uint32_t stream_id, std::span<uint8_t> out)
{
    if (out.size() < ALT_CONTEXT_SIZE) {
        throw std::invalid_argument(
            "Destination span size (" + std::to_string(out.size()) +
            ") is smaller than alternative context size (" +
            std::to_string(ALT_CONTEXT_SIZE) + ")");
    }

    std::fill(out.begin(), out.end(), 0);

    uint32_t header = ALT_CONTEXT_STATIC_BITS |
                      (static_cast<uint32_t>(pkt_n & 0x0f) << 16) |
                      static_cast<uint32_t>(out.size() / 4);

    pack_u32(&out[0], header);
    pack_u32(&out[4], stream_id);

    int idx = 0;
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]], ctx.class_id);
    pack_u32(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             ctx.cif != 0 ? ctx.cif : static_cast<uint32_t>(cif_mask::alternative_fields));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.bandwidth));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.if_ref_freq));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.rf_ref_freq));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.if_band_offset));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             double_to_vita_fixed(ctx.sample_rate));
    pack_u32(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             ctx.state_and_event_indicators != 0
                 ? ctx.state_and_event_indicators
                 : static_cast<uint32_t>(state_and_event::default_indicators));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]], ctx.payload_format);
}

void difi::pack_context(const standard_context& ctx,
                        uint8_t pkt_n,
                        uint32_t stream_id,
                        std::span<uint8_t> out)
{
    pack_context<context_version::standard>(ctx, pkt_n, stream_id, out);
}

void difi::pack_context(const alternative_context& ctx,
                        uint8_t pkt_n,
                        uint32_t stream_id,
                        std::span<uint8_t> out)
{
    pack_context<context_version::alternative>(ctx, pkt_n, stream_id, out);
}

void difi::pack_context(const context_variant& ctx,
                        uint8_t pkt_n,
                        uint32_t stream_id,
                        std::span<uint8_t> out)
{
    std::visit([&](const auto& c) { pack_context(c, pkt_n, stream_id, out); }, ctx);
}

template <>
std::vector<uint8_t> difi::pack_context<difi::context_version::standard>(
    const standard_context& ctx, uint8_t pkt_n, uint32_t stream_id, size_t packet_size)
{
    std::vector<uint8_t> out(packet_size, 0);
    pack_context<context_version::standard>(ctx, pkt_n, stream_id, std::span<uint8_t>(out));
    return out;
}

template <>
std::vector<uint8_t> difi::pack_context<difi::context_version::alternative>(
    const alternative_context& ctx, uint8_t pkt_n, uint32_t stream_id, size_t packet_size)
{
    std::vector<uint8_t> out(packet_size, 0);
    pack_context<context_version::alternative>(ctx, pkt_n, stream_id, std::span<uint8_t>(out));
    return out;
}

std::vector<uint8_t> difi::pack_context(const standard_context& ctx,
                                        uint8_t pkt_n,
                                        uint32_t stream_id,
                                        size_t packet_size)
{
    return pack_context<context_version::standard>(ctx, pkt_n, stream_id, packet_size);
}

std::vector<uint8_t> difi::pack_context(const alternative_context& ctx,
                                        uint8_t pkt_n,
                                        uint32_t stream_id,
                                        size_t packet_size)
{
    return pack_context<context_version::alternative>(ctx, pkt_n, stream_id, packet_size);
}

std::vector<uint8_t>
difi::pack_context(const context_variant& ctx, uint8_t pkt_n, uint32_t stream_id)
{
    return std::visit([&](const auto& c) { return pack_context(c, pkt_n, stream_id); },
                      ctx);
}

template <>
difi::standard_context
difi::make_context<difi::context_version::standard>(const difi::context_config& cfg)
{
    standard_context ctx;
    ctx.class_id = (cfg.class_id != 0)
                       ? cfg.class_id
                       : make_class_id(oui::difi, packet_class::standard_context);
    ctx.full = cfg.full;
    ctx.frac = cfg.frac;
    ctx.cif = (cfg.cif != 0) ? cfg.cif : static_cast<uint32_t>(cif_mask::standard_with_change);
    ctx.ref_point = (cfg.ref_point != reference_point::unspecified)
                        ? cfg.ref_point
                        : reference_point::if_converter;
    ctx.sample_rate = cfg.sample_rate;
    ctx.bandwidth = (cfg.bandwidth > 0.0) ? cfg.bandwidth : cfg.sample_rate;
    ctx.if_ref_freq = cfg.if_freq;
    ctx.rf_ref_freq = cfg.rf_freq;
    ctx.if_band_offset = cfg.if_band_offset;
    ctx.ref_level = cfg.ref_level;
    ctx.rf_gain = cfg.rf_gain;
    ctx.if_gain = cfg.if_gain;
    ctx.timestamp_adjustment = cfg.timestamp_adjustment;
    ctx.timestamp_calibration_time =
        (cfg.timestamp_calibration_time != 0) ? cfg.timestamp_calibration_time : cfg.full;
    ctx.state_and_event_indicators =
        (cfg.state_and_event_indicators != 0)
            ? cfg.state_and_event_indicators
            : static_cast<uint32_t>(state_and_event::default_indicators);
    ctx.payload_format = (cfg.depth == 8)
                             ? static_cast<uint64_t>(payload_format::sc8_link_efficient)
                             : static_cast<uint64_t>(payload_format::sc16_link_efficient);
    ctx.tsi = cfg.tsi;
    ctx.packet_size = STANDARD_CONTEXT_SIZE;

    auto packed = pack_context(ctx, 0, cfg.stream_id, STANDARD_CONTEXT_SIZE);
    ctx.raw.assign(packed.begin(), packed.end());

    return ctx;
}

template <>
difi::alternative_context
difi::make_context<difi::context_version::alternative>(const difi::context_config& cfg)
{
    alternative_context ctx;
    ctx.class_id = (cfg.class_id != 0)
                       ? cfg.class_id
                       : make_class_id(oui::vita, packet_class::standard_context);
    ctx.cif = (cfg.cif != 0) ? cfg.cif : static_cast<uint32_t>(cif_mask::alternative_fields);
    ctx.sample_rate = cfg.sample_rate;
    ctx.bandwidth = (cfg.bandwidth > 0.0) ? cfg.bandwidth : cfg.sample_rate;
    ctx.if_ref_freq = cfg.if_freq;
    ctx.rf_ref_freq = cfg.rf_freq;
    ctx.if_band_offset = cfg.if_band_offset;
    ctx.state_and_event_indicators =
        (cfg.state_and_event_indicators != 0)
            ? cfg.state_and_event_indicators
            : static_cast<uint32_t>(state_and_event::default_indicators);
    ctx.payload_format = (cfg.depth == 8)
                             ? static_cast<uint64_t>(payload_format::sc8_link_efficient)
                             : static_cast<uint64_t>(payload_format::sc16_link_efficient);
    ctx.packet_size = ALT_CONTEXT_SIZE;

    auto packed = pack_context(ctx, 0, cfg.stream_id, ALT_CONTEXT_SIZE);
    ctx.raw.assign(packed.begin(), packed.end());

    return ctx;
}

difi::context_variant difi::make_context(context_version version,
                                         const context_config& cfg)
{
    if (version == context_version::alternative) {
        return make_context<context_version::alternative>(cfg);
    }
    return make_context<context_version::standard>(cfg);
}

std::optional<difi::flow_control> difi::parse_flow_control(const void* buf, size_t len)
{
    if (!buf || len < FLOW_CONTROL_SIZE) {
        return std::nullopt;
    }

    const uint8_t* in = static_cast<const uint8_t*>(buf);
    uint32_t w0 = unpack_u32(&in[0]);
    uint8_t type_val = static_cast<uint8_t>((w0 >> 28) & 0x0f);
    if (type_val != static_cast<uint8_t>(packet_type::command)) {
        return std::nullopt;
    }

    uint16_t size_words = static_cast<uint16_t>(w0 & 0xffff);
    if (size_words < FLOW_CONTROL_WORDS) {
        return std::nullopt;
    }

    flow_control fc;
    fc.type = packet_type::command;
    fc.pkt_n = static_cast<uint8_t>((w0 >> 16) & 0x0f);
    fc.tsi = static_cast<tsi_type>((w0 >> 22) & 0x03);
    fc.tsf = static_cast<tsf_type>((w0 >> 20) & 0x03);

    fc.stream_id = unpack_u32(&in[4]);
    fc.class_id = unpack_u64(&in[8]);
    fc.message_id = unpack_u32(&in[16]);
    fc.controllee_id = unpack_u32(&in[20]);
    fc.controller_id = unpack_u32(&in[24]);
    fc.full = unpack_u32(&in[28]);
    fc.frac = unpack_u64(&in[32]);
    // in[40..43] is reserved / CAM
    fc.cif0 = unpack_u32(&in[44]);
    fc.cif1 = unpack_u32(&in[48]);
    fc.ref_point = static_cast<reference_point>(unpack_u32(&in[52]));

    // Sample rate: 64-bit VITA float with integer radix at bit 20
    uint64_t sr_raw = unpack_u64(&in[56]);
    fc.sample_rate = static_cast<double>(sr_raw) / static_cast<double>(1ULL << 20);

    // Timestamp adjustment in femtoseconds
    fc.timestamp_adjustment = static_cast<int64_t>(unpack_u64(&in[64]));

    // Buffer size in bytes
    fc.buffer_size = unpack_u64(&in[72]);

    // Word 20: Buffer Level (bits 15-4) and Discrete Indicators (bits 3-0)
    uint32_t w20 = unpack_u32(&in[80]);
    fc.buffer_level = static_cast<uint16_t>((w20 >> 4) & 0x0fff);
    fc.overflow = (w20 & 0x8) != 0;
    fc.nearly_full = (w20 & 0x4) != 0;
    fc.nearly_empty = (w20 & 0x2) != 0;
    fc.underflow = (w20 & 0x1) != 0;

    return fc;
}

std::optional<difi::flow_control> difi::parse_flow_control(std::span<const uint8_t> span)
{
    return parse_flow_control(span.data(), span.size());
}

void difi::pack_flow_control(const flow_control& fc, std::span<uint8_t> out)
{
    if (out.size() < FLOW_CONTROL_SIZE) {
        throw std::invalid_argument(
            "Destination span too small for flow control packet (need 84 bytes)");
    }

    std::fill(out.begin(), out.begin() + FLOW_CONTROL_SIZE, 0);
    uint8_t* p = out.data();

    uint32_t static_bits = 0x68000000U |
                           ((static_cast<uint32_t>(fc.tsi) & 0x3U) << 22) |
                           ((static_cast<uint32_t>(fc.tsf) & 0x3U) << 20);
    uint32_t w0 = static_bits |
                  ((static_cast<uint32_t>(fc.pkt_n) & 0x0fU) << 16) |
                  static_cast<uint32_t>(FLOW_CONTROL_WORDS);

    pack_u32(&p[0], w0);
    pack_u32(&p[4], fc.stream_id);

    uint64_t cid = fc.class_id;
    if (cid == 0) {
        cid = make_class_id(oui::difi,
                            packet_class::real_time_flow_control,
                            information_class::sample_count_data_plane);
    }
    pack_u64(&p[8], cid);
    pack_u32(&p[16], fc.message_id);
    pack_u32(&p[20], fc.controllee_id);
    pack_u32(&p[24], fc.controller_id);
    pack_u32(&p[28], fc.full);
    pack_u64(&p[32], fc.frac);
    pack_u32(&p[40], 0); // Reserved

    uint32_t cif0 = fc.cif0 != 0 ? fc.cif0 : 0x40300002U;
    uint32_t cif1 = fc.cif1 != 0 ? fc.cif1 : 0x00000002U;
    pack_u32(&p[44], cif0);
    pack_u32(&p[48], cif1);
    pack_u32(&p[52], static_cast<uint32_t>(fc.ref_point));

    uint64_t sr_raw = static_cast<uint64_t>(fc.sample_rate * static_cast<double>(1ULL << 20));
    pack_u64(&p[56], sr_raw);
    pack_u64(&p[64], static_cast<uint64_t>(fc.timestamp_adjustment));
    pack_u64(&p[72], fc.buffer_size);

    uint32_t w20 = ((static_cast<uint32_t>(fc.buffer_level) & 0x0fffU) << 4) |
                   (fc.overflow ? 0x8U : 0U) |
                   (fc.nearly_full ? 0x4U : 0U) |
                   (fc.nearly_empty ? 0x2U : 0U) |
                   (fc.underflow ? 0x1U : 0U);
    pack_u32(&p[80], w20);
}

std::vector<uint8_t> difi::pack_flow_control(const flow_control& fc)
{
    std::vector<uint8_t> out(FLOW_CONTROL_SIZE);
    pack_flow_control(fc, std::span<uint8_t>(out));
    return out;
}

std::pair<uint32_t, uint64_t> difi::system_timestamp(uint32_t lead_time_us, tsi_type tsi)
{
    uint32_t full = 0;
    uint64_t frac = 0;

    auto calc = [&](auto now) {
        auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(now);
        auto frac_duration = now - now_s;
        full = static_cast<uint32_t>(now_s.time_since_epoch().count());
        frac = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(frac_duration).count()) *
            1000ULL;
    };

    switch (tsi) {
    case tsi_type::utc:
        calc(std::chrono::utc_clock::now());
        break;
    case tsi_type::gps:
        calc(std::chrono::gps_clock::now());
        break;
    case tsi_type::posix:
    case tsi_type::none:
    default:
        calc(std::chrono::system_clock::now());
        break;
    }

    if (lead_time_us > 0) {
        frac += static_cast<uint64_t>(lead_time_us) * 1000000ULL;
        if (frac >= PICO_PER_SEC) {
            full += static_cast<uint32_t>(frac / PICO_PER_SEC);
            frac = frac % PICO_PER_SEC;
        }
    }

    return { full, frac };
}

template <>
difi::standard_context difi::make_default_context<difi::context_version::standard>(
    double samp_rate, int depth, uint32_t stream_id, uint32_t full, uint64_t frac)
{
    standard_context ctx;
    ctx.class_id = make_class_id(oui::difi, packet_class::standard_context);
    ctx.full = full;
    ctx.frac = frac;
    ctx.cif = static_cast<uint32_t>(cif_mask::standard_with_change);
    ctx.ref_point = reference_point::if_converter;
    ctx.bandwidth = samp_rate * 0.8;
    ctx.if_ref_freq = 100000000.0;
    ctx.rf_ref_freq = 7500000000.0;
    ctx.if_band_offset = -0.1 * samp_rate;
    ctx.ref_level = 20.0f;
    ctx.rf_gain = 14.2f;
    ctx.if_gain = -1.3f;
    ctx.sample_rate = samp_rate;
    ctx.timestamp_adjustment = static_cast<int64_t>(1e-5 * 1e15); // 10 us in fs
    ctx.timestamp_calibration_time = full;
    ctx.state_and_event_indicators = static_cast<uint32_t>(state_and_event::default_indicators);
    ctx.payload_format =
        (depth == 8) ? static_cast<uint64_t>(payload_format::sc8_link_efficient)
                     : static_cast<uint64_t>(payload_format::sc16_link_efficient);
    ctx.packet_size = STANDARD_CONTEXT_SIZE;

    auto packed = pack_context(ctx, 0, stream_id, STANDARD_CONTEXT_SIZE);
    ctx.raw.assign(packed.begin(), packed.end());

    return ctx;
}

template <>
difi::alternative_context difi::make_default_context<difi::context_version::alternative>(
    double samp_rate, int depth, uint32_t stream_id, uint32_t full, uint64_t frac)
{
    (void)full;
    (void)frac;
    alternative_context ctx;
    ctx.class_id = make_class_id(oui::vita, packet_class::standard_context);
    ctx.cif = static_cast<uint32_t>(cif_mask::alternative_fields);
    ctx.bandwidth = samp_rate * 0.8;
    ctx.if_ref_freq = 0.0;
    ctx.rf_ref_freq = 0.0;
    ctx.if_band_offset = 0.0;
    ctx.sample_rate = samp_rate;
    ctx.state_and_event_indicators = static_cast<uint32_t>(state_and_event::default_indicators);
    ctx.payload_format =
        (depth == 8) ? static_cast<uint64_t>(payload_format::sc8_link_efficient)
                     : static_cast<uint64_t>(payload_format::sc16_link_efficient);
    ctx.packet_size = ALT_CONTEXT_SIZE;

    auto packed = pack_context(ctx, 0, stream_id, ALT_CONTEXT_SIZE);
    ctx.raw.assign(packed.begin(), packed.end());

    return ctx;
}

difi::standard_context difi::make_default_context(double samp_rate,
                                                  int depth,
                                                  uint32_t stream_id,
                                                  uint32_t full,
                                                  uint64_t frac,
                                                  size_t packet_size)
{
    if (packet_size == ALT_CONTEXT_SIZE) {
        standard_context ctx;
        ctx.class_id = make_class_id(oui::vita, packet_class::standard_context);
        ctx.full = full;
        ctx.frac = frac;
        ctx.cif = static_cast<uint32_t>(cif_mask::alternative_fields);
        ctx.ref_point = reference_point::if_converter;
        ctx.bandwidth = samp_rate * 0.8;
        ctx.if_ref_freq = 100000000.0;
        ctx.rf_ref_freq = 7500000000.0;
        ctx.if_band_offset = -0.1 * samp_rate;
        ctx.ref_level = 20.0f;
        ctx.rf_gain = 14.2f;
        ctx.if_gain = -1.3f;
        ctx.sample_rate = samp_rate;
        ctx.timestamp_adjustment = static_cast<int64_t>(1e-5 * 1e15);
        ctx.timestamp_calibration_time = full;
        ctx.state_and_event_indicators = static_cast<uint32_t>(DEFAULT_STATE_AND_EVENTS);
        ctx.payload_format = (depth == 8) ? EIGHT_BIT_SIGNED_CART_LINK_EFF
                                          : SIXTEEN_BIT_SIGNED_CART_LINK_EFF;
        ctx.packet_size = ALT_CONTEXT_SIZE;

        alternative_context alt_ctx;
        alt_ctx.class_id = ctx.class_id;
        alt_ctx.cif = ctx.cif;
        alt_ctx.bandwidth = ctx.bandwidth;
        alt_ctx.sample_rate = ctx.sample_rate;
        alt_ctx.state_and_event_indicators = ctx.state_and_event_indicators;
        alt_ctx.payload_format = ctx.payload_format;
        alt_ctx.packet_size = ALT_CONTEXT_SIZE;
        auto packed = pack_context(alt_ctx, 0, stream_id, ALT_CONTEXT_SIZE);
        ctx.raw.assign(packed.begin(), packed.end());
        return ctx;
    }

    return make_default_context<context_version::standard>(
        samp_rate, depth, stream_id, full, frac);
}

std::pair<uint32_t, uint64_t> difi::advance_timestamp(uint32_t full,
                                                      uint64_t frac,
                                                      size_t num_samples,
                                                      double samp_rate)
{
    if (samp_rate <= 0.0 || num_samples == 0) {
        return { full, frac };
    }
    double delta_sec = static_cast<double>(num_samples) / samp_rate;
    uint32_t full_sec = static_cast<uint32_t>(delta_sec);
    double rem_sec = delta_sec - static_cast<double>(full_sec);
    uint64_t delta_pico = static_cast<uint64_t>(rem_sec * PICO_PER_SEC);

    frac += delta_pico;
    if (frac >= PICO_PER_SEC) {
        full_sec += static_cast<uint32_t>(frac / PICO_PER_SEC);
        frac = frac % PICO_PER_SEC;
    }
    full += full_sec;
    return { full, frac };
}

template <sample_type T>
void difi::pack_samples(const T* in, size_t num_samples, void* payload, int depth)
{
    if (num_samples == 0 || in == nullptr || payload == nullptr) {
        return;
    }

    if constexpr (std::is_same_v<T, gr_complex>) {
        const float* src_f = reinterpret_cast<const float*>(in);
        if (depth == 16) {
            int16_t* out16 = static_cast<int16_t*>(payload);
            volk_32f_s32f_convert_16i(
                out16, src_f, 1.0f, static_cast<unsigned int>(num_samples * 2));
            swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
        } else {
            int8_t* out8 = static_cast<int8_t*>(payload);
            volk_32f_s32f_convert_8i(
                out8, src_f, 1.0f, static_cast<unsigned int>(num_samples * 2));
        }
    } else if constexpr (std::is_same_v<T, std::complex<int16_t>>) {
        if (depth == 16) {
            std::memcpy(payload, in, num_samples * sizeof(T));
            swap_endian_16(reinterpret_cast<uint16_t*>(payload), num_samples * 2);
        } else {
            const int16_t* in16 = reinterpret_cast<const int16_t*>(in);
            int8_t* out8 = static_cast<int8_t*>(payload);
            volk_16i_convert_8i(out8, in16, static_cast<unsigned int>(num_samples * 2));
        }
    } else { // T is std::complex<char> or std::complex<int8_t>
        const int8_t* in8 = reinterpret_cast<const int8_t*>(in);
        if (depth == 16) {
            int16_t* out16 = static_cast<int16_t*>(payload);
            volk_8i_convert_16i(out16, in8, static_cast<unsigned int>(num_samples * 2));
            swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
        } else {
            std::memcpy(payload, in, num_samples * sizeof(T));
        }
    }
}

template <sample_type T>
void difi::pack_samples(const T* in,
                        size_t num_samples,
                        void* payload,
                        int depth,
                        float gain,
                        gr_complex offset)
{
    if (num_samples == 0 || in == nullptr || payload == nullptr) {
        return;
    }

    if constexpr (std::is_same_v<T, gr_complex>) {
        const float* src_f = reinterpret_cast<const float*>(in);
        std::vector<float> temp_f;

        temp_f.resize(num_samples * 2);
        std::vector<gr_complex> offset_vec(num_samples, offset);
        volk_32fc_x2_add_32fc(reinterpret_cast<lv_32fc_t*>(temp_f.data()),
                              reinterpret_cast<const lv_32fc_t*>(src_f),
                              reinterpret_cast<const lv_32fc_t*>(offset_vec.data()),
                              static_cast<unsigned int>(num_samples));
        src_f = temp_f.data();

        if (depth == 16) {
            int16_t* out16 = static_cast<int16_t*>(payload);
            volk_32f_s32f_convert_16i(
                out16, src_f, gain, static_cast<unsigned int>(num_samples * 2));
            swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
        } else {
            int8_t* out8 = static_cast<int8_t*>(payload);
            volk_32f_s32f_convert_8i(
                out8, src_f, gain, static_cast<unsigned int>(num_samples * 2));
        }
    } else if constexpr (std::is_same_v<T, std::complex<int16_t>>) {
        const int16_t* in16 = reinterpret_cast<const int16_t*>(in);
        std::vector<float> temp_f(num_samples * 2);
        volk_16i_s32f_convert_32f(
            temp_f.data(), in16, 1.0f, static_cast<unsigned int>(num_samples * 2));

        std::vector<gr_complex> offset_vec(num_samples, offset);
        volk_32fc_x2_add_32fc(reinterpret_cast<lv_32fc_t*>(temp_f.data()),
                              reinterpret_cast<const lv_32fc_t*>(temp_f.data()),
                              reinterpret_cast<const lv_32fc_t*>(offset_vec.data()),
                              static_cast<unsigned int>(num_samples));
        if (depth == 16) {
            int16_t* out16 = static_cast<int16_t*>(payload);
            volk_32f_s32f_convert_16i(
                out16, temp_f.data(), gain, static_cast<unsigned int>(num_samples * 2));
            swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
        } else {
            int8_t* out8 = static_cast<int8_t*>(payload);
            volk_32f_s32f_convert_8i(
                out8, temp_f.data(), gain, static_cast<unsigned int>(num_samples * 2));
        }
    } else { // T is std::complex<char> or std::complex<int8_t>
        const int8_t* in8 = reinterpret_cast<const int8_t*>(in);
        std::vector<float> temp_f(num_samples * 2);
        volk_8i_s32f_convert_32f(
            temp_f.data(), in8, 1.0f, static_cast<unsigned int>(num_samples * 2));

        std::vector<gr_complex> offset_vec(num_samples, offset);
        volk_32fc_x2_add_32fc(reinterpret_cast<lv_32fc_t*>(temp_f.data()),
                              reinterpret_cast<const lv_32fc_t*>(temp_f.data()),
                              reinterpret_cast<const lv_32fc_t*>(offset_vec.data()),
                              static_cast<unsigned int>(num_samples));
        if (depth == 16) {
            int16_t* out16 = static_cast<int16_t*>(payload);
            volk_32f_s32f_convert_16i(
                out16, temp_f.data(), gain, static_cast<unsigned int>(num_samples * 2));
            swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
        } else {
            int8_t* out8 = static_cast<int8_t*>(payload);
            volk_32f_s32f_convert_8i(
                out8, temp_f.data(), gain, static_cast<unsigned int>(num_samples * 2));
        }
    }
}

template <sample_type T>
size_t difi::unpack_samples(
    const void* payload, size_t payload_bytes, T* out, size_t max_samples, int depth)
{
    if (payload == nullptr || out == nullptr || max_samples == 0) {
        return 0;
    }

    size_t bytes_per_sample = (depth == 16) ? 4 : 2;
    size_t available = payload_bytes / bytes_per_sample;
    size_t count = std::min(available, max_samples);
    if (count == 0) {
        return 0;
    }

    if constexpr (std::is_same_v<T, gr_complex>) {
        float* out_f = reinterpret_cast<float*>(out);
        if (depth == 16) {
            const int16_t* in16 = static_cast<const int16_t*>(payload);
            std::vector<int16_t> buf(count * 2);
            std::memcpy(buf.data(), in16, count * 4);
            swap_endian_16(reinterpret_cast<uint16_t*>(buf.data()), count * 2);
            volk_16i_s32f_convert_32f(
                out_f, buf.data(), 1.0f, static_cast<unsigned int>(count * 2));
        } else {
            const int8_t* in8 = static_cast<const int8_t*>(payload);
            volk_8i_s32f_convert_32f(
                out_f, in8, 1.0f, static_cast<unsigned int>(count * 2));
        }
    } else if constexpr (std::is_same_v<T, std::complex<int16_t>>) {
        if (depth == 16) {
            std::memcpy(out, payload, count * sizeof(T));
            swap_endian_16(reinterpret_cast<uint16_t*>(out), count * 2);
        } else {
            const int8_t* in8 = static_cast<const int8_t*>(payload);
            int16_t* out16 = reinterpret_cast<int16_t*>(out);
            volk_8i_convert_16i(out16, in8, static_cast<unsigned int>(count * 2));
        }
    } else { // T is std::complex<char> or std::complex<int8_t>
        int8_t* out8 = reinterpret_cast<int8_t*>(out);
        if (depth == 16) {
            const int16_t* in16 = static_cast<const int16_t*>(payload);
            std::vector<int16_t> buf(count * 2);
            std::memcpy(buf.data(), in16, count * 4);
            swap_endian_16(reinterpret_cast<uint16_t*>(buf.data()), count * 2);
            volk_16i_convert_8i(out8, buf.data(), static_cast<unsigned int>(count * 2));
        } else {
            std::memcpy(out, payload, count * sizeof(T));
        }
    }

    return count;
}

pmt::pmt_t difi::context_to_pmt(const header& header, const standard_context& ctx)
{
    pmt::pmt_t dict = pmt::make_dict();
    dict = pmt::dict_add(dict, pmt::intern("header"), pmt::from_long(header.raw_header));
    dict = pmt::dict_add(
        dict, pmt::intern("stream_num"), pmt::from_uint64(header.stream_id));
    dict = pmt::dict_add(dict, pmt::intern("class_id"), pmt::from_long(ctx.class_id));
    dict = pmt::dict_add(dict, pmt::intern("full"), pmt::from_long(ctx.full));
    dict = pmt::dict_add(dict, pmt::intern("frac"), pmt::from_uint64(ctx.frac));
    dict = pmt::dict_add(dict, pmt::intern("CIF"), pmt::from_long(ctx.cif));
    dict = pmt::dict_add(dict, pmt::intern("bandwidth"), pmt::from_double(ctx.bandwidth));
    dict = pmt::dict_add(
        dict, pmt::intern("if_reference_frequency"), pmt::from_double(ctx.if_ref_freq));
    dict = pmt::dict_add(
        dict, pmt::intern("rf_reference_frequency"), pmt::from_double(ctx.rf_ref_freq));
    dict = pmt::dict_add(
        dict, pmt::intern("if_band_offset"), pmt::from_double(ctx.if_band_offset));
    dict =
        pmt::dict_add(dict, pmt::intern("samp_rate"), pmt::from_double(ctx.sample_rate));
    dict = pmt::dict_add(dict,
                         pmt::intern("state_and_event_indicator"),
                         pmt::from_long(ctx.state_and_event_indicators));
    dict = pmt::dict_add(dict,
                         pmt::intern("data_packet_payload_format"),
                         pmt::from_uint64(ctx.payload_format));

    if (!ctx.raw.empty()) {
        dict = pmt::dict_add(
            dict, pmt::intern("raw"), pmt::init_s8vector(ctx.raw.size(), ctx.raw.data()));
    }

    if (ctx.packet_size != ALT_CONTEXT_SIZE) {
        dict = pmt::dict_add(
            dict, pmt::intern("reference_point"), pmt::from_long(static_cast<uint32_t>(ctx.ref_point)));
        dict = pmt::dict_add(
            dict, pmt::intern("reference_level"), pmt::from_float(ctx.ref_level));
        dict = pmt::dict_add(dict, pmt::intern("rf_gain"), pmt::from_float(ctx.rf_gain));
        dict = pmt::dict_add(dict, pmt::intern("if_gain"), pmt::from_float(ctx.if_gain));
        dict = pmt::dict_add(dict,
                             pmt::intern("timestamp_adjustment"),
                             pmt::from_long(ctx.timestamp_adjustment));
        dict = pmt::dict_add(dict,
                             pmt::intern("timestamp_calibration_time"),
                             pmt::from_uint64(ctx.timestamp_calibration_time));
        dict = pmt::dict_add(dict,
                             pmt::intern("tsi"),
                             pmt::from_long(static_cast<int>(ctx.tsi)));
    }

    return dict;
}

pmt::pmt_t difi::context_to_pmt(const header& header, const alternative_context& ctx)
{
    pmt::pmt_t dict = pmt::make_dict();
    dict = pmt::dict_add(dict, pmt::intern("header"), pmt::from_long(header.raw_header));
    dict = pmt::dict_add(
        dict, pmt::intern("stream_num"), pmt::from_uint64(header.stream_id));
    dict = pmt::dict_add(dict, pmt::intern("class_id"), pmt::from_long(ctx.class_id));
    dict = pmt::dict_add(dict, pmt::intern("CIF"), pmt::from_long(ctx.cif));
    dict = pmt::dict_add(dict, pmt::intern("bandwidth"), pmt::from_double(ctx.bandwidth));
    dict = pmt::dict_add(
        dict, pmt::intern("if_reference_frequency"), pmt::from_double(ctx.if_ref_freq));
    dict = pmt::dict_add(
        dict, pmt::intern("rf_reference_frequency"), pmt::from_double(ctx.rf_ref_freq));
    dict = pmt::dict_add(
        dict, pmt::intern("if_band_offset"), pmt::from_double(ctx.if_band_offset));
    dict =
        pmt::dict_add(dict, pmt::intern("samp_rate"), pmt::from_double(ctx.sample_rate));
    dict = pmt::dict_add(dict,
                         pmt::intern("state_and_event_indicator"),
                         pmt::from_long(ctx.state_and_event_indicators));
    dict = pmt::dict_add(dict,
                         pmt::intern("data_packet_payload_format"),
                         pmt::from_uint64(ctx.payload_format));

    if (!ctx.raw.empty()) {
        dict = pmt::dict_add(
            dict, pmt::intern("raw"), pmt::init_s8vector(ctx.raw.size(), ctx.raw.data()));
    }

    return dict;
}

pmt::pmt_t difi::context_to_pmt(const header& header, const context_variant& ctx)
{
    return std::visit([&](const auto& c) { return context_to_pmt(header, c); }, ctx);
}

template <>
difi::standard_context
difi::pmt_to_context<difi::context_version::standard>(pmt::pmt_t dict)
{
    standard_context ctx;
    if (!pmt::is_dict(dict)) {
        return ctx;
    }

    auto get_val = [&](const char* key) {
        return pmt::dict_ref(dict, pmt::intern(key), pmt::get_PMT_NIL());
    };

    auto p_raw = get_val("raw");
    if (pmt::is_s8vector(p_raw)) {
        ctx.raw = pmt::s8vector_elements(p_raw);
        ctx.packet_size = ctx.raw.size();
    }

    auto p_class_id = get_val("class_id");
    if (pmt::is_integer(p_class_id)) {
        ctx.class_id = static_cast<uint64_t>(pmt::to_long(p_class_id));
    }

    auto p_full = get_val("full");
    if (pmt::is_integer(p_full)) {
        ctx.full = static_cast<uint32_t>(pmt::to_long(p_full));
    }

    auto p_frac = get_val("frac");
    if (pmt::is_uint64(p_frac)) {
        ctx.frac = pmt::to_uint64(p_frac);
    }

    auto p_cif = get_val("CIF");
    if (pmt::is_integer(p_cif)) {
        ctx.cif = static_cast<uint32_t>(pmt::to_long(p_cif));
    }

    auto p_bw = get_val("bandwidth");
    if (pmt::is_real(p_bw)) {
        ctx.bandwidth = pmt::to_double(p_bw);
    }

    auto p_if_ref = get_val("if_reference_frequency");
    if (pmt::is_real(p_if_ref)) {
        ctx.if_ref_freq = pmt::to_double(p_if_ref);
    }

    auto p_rf_ref = get_val("rf_reference_frequency");
    if (pmt::is_real(p_rf_ref)) {
        ctx.rf_ref_freq = pmt::to_double(p_rf_ref);
    }

    auto p_if_offset = get_val("if_band_offset");
    if (pmt::is_real(p_if_offset)) {
        ctx.if_band_offset = pmt::to_double(p_if_offset);
    }

    auto p_rate = get_val("samp_rate");
    if (pmt::is_real(p_rate)) {
        ctx.sample_rate = pmt::to_double(p_rate);
    }

    auto p_ref_lvl = get_val("reference_level");
    if (pmt::is_real(p_ref_lvl)) {
        ctx.ref_level = static_cast<float>(pmt::to_double(p_ref_lvl));
    }

    auto p_ref_point = get_val("reference_point");
    if (pmt::is_integer(p_ref_point)) {
        ctx.ref_point = static_cast<reference_point>(pmt::to_long(p_ref_point));
    }

    auto p_tsi = get_val("tsi");
    if (pmt::is_integer(p_tsi)) {
        ctx.tsi = static_cast<tsi_type>(pmt::to_long(p_tsi));
    }

    auto p_adj = get_val("timestamp_adjustment");
    if (pmt::is_integer(p_adj)) {
        ctx.timestamp_adjustment = pmt::to_long(p_adj);
    }

    auto p_cal = get_val("timestamp_calibration_time");
    if (pmt::is_uint64(p_cal)) {
        ctx.timestamp_calibration_time = static_cast<uint32_t>(pmt::to_uint64(p_cal));
    } else if (pmt::is_integer(p_cal)) {
        ctx.timestamp_calibration_time = static_cast<uint32_t>(pmt::to_long(p_cal));
    }

    auto p_rf_gain = get_val("rf_gain");
    if (pmt::is_real(p_rf_gain)) {
        ctx.rf_gain = static_cast<float>(pmt::to_double(p_rf_gain));
    }

    auto p_if_gain = get_val("if_gain");
    if (pmt::is_real(p_if_gain)) {
        ctx.if_gain = static_cast<float>(pmt::to_double(p_if_gain));
    }

    auto p_state = get_val("state_and_event_indicator");
    if (pmt::is_integer(p_state)) {
        ctx.state_and_event_indicators = static_cast<uint32_t>(pmt::to_long(p_state));
    }

    auto p_format = get_val("data_packet_payload_format");
    if (pmt::is_uint64(p_format)) {
        ctx.payload_format = pmt::to_uint64(p_format);
    }

    return ctx;
}

template <>
difi::alternative_context
difi::pmt_to_context<difi::context_version::alternative>(pmt::pmt_t dict)
{
    alternative_context ctx;
    if (!pmt::is_dict(dict)) {
        return ctx;
    }

    auto get_val = [&](const char* key) {
        return pmt::dict_ref(dict, pmt::intern(key), pmt::get_PMT_NIL());
    };

    auto p_raw = get_val("raw");
    if (pmt::is_s8vector(p_raw)) {
        ctx.raw = pmt::s8vector_elements(p_raw);
        ctx.packet_size = ctx.raw.size();
    }

    auto p_class_id = get_val("class_id");
    if (pmt::is_integer(p_class_id)) {
        ctx.class_id = static_cast<uint64_t>(pmt::to_long(p_class_id));
    }

    auto p_cif = get_val("CIF");
    if (pmt::is_integer(p_cif)) {
        ctx.cif = static_cast<uint32_t>(pmt::to_long(p_cif));
    }

    auto p_bw = get_val("bandwidth");
    if (pmt::is_real(p_bw)) {
        ctx.bandwidth = pmt::to_double(p_bw);
    }

    auto p_if_ref = get_val("if_reference_frequency");
    if (pmt::is_real(p_if_ref)) {
        ctx.if_ref_freq = pmt::to_double(p_if_ref);
    }

    auto p_rf_ref = get_val("rf_reference_frequency");
    if (pmt::is_real(p_rf_ref)) {
        ctx.rf_ref_freq = pmt::to_double(p_rf_ref);
    }

    auto p_if_offset = get_val("if_band_offset");
    if (pmt::is_real(p_if_offset)) {
        ctx.if_band_offset = pmt::to_double(p_if_offset);
    }

    auto p_rate = get_val("samp_rate");
    if (pmt::is_real(p_rate)) {
        ctx.sample_rate = pmt::to_double(p_rate);
    }

    auto p_state = get_val("state_and_event_indicator");
    if (pmt::is_integer(p_state)) {
        ctx.state_and_event_indicators = static_cast<uint32_t>(pmt::to_long(p_state));
    }

    auto p_format = get_val("data_packet_payload_format");
    if (pmt::is_uint64(p_format)) {
        ctx.payload_format = pmt::to_uint64(p_format);
    }

    return ctx;
}

difi::standard_context difi::pmt_to_context(pmt::pmt_t dict)
{
    return pmt_to_context<context_version::standard>(dict);
}

pmt::pmt_t
difi::make_pkt_n_dict(uint8_t pkt_n, size_t data_len, uint32_t full, uint64_t frac)
{
    pmt::pmt_t dict = pmt::make_dict();
    dict = pmt::dict_add(
        dict, pmt::intern("pck_n"), pmt::from_uint64(static_cast<uint64_t>(pkt_n)));
    dict = pmt::dict_add(
        dict, pmt::intern("data_len"), pmt::from_uint64(static_cast<uint64_t>(data_len)));
    dict =
        pmt::dict_add(dict, pmt::intern("full"), pmt::from_long(static_cast<long>(full)));
    dict = pmt::dict_add(dict, pmt::intern("frac"), pmt::from_uint64(frac));
    return dict;
}

template void difi::pack_samples<gr_complex>(const gr_complex*, size_t, void*, int);
template void
difi::pack_samples<std::complex<char>>(const std::complex<char>*, size_t, void*, int);
template void
difi::pack_samples<std::complex<int8_t>>(const std::complex<int8_t>*, size_t, void*, int);
template void
difi::pack_samples<std::complex<int16_t>>(const std::complex<int16_t>*, size_t, void*, int);

template void
difi::pack_samples<gr_complex>(const gr_complex*, size_t, void*, int, float, gr_complex);
template void difi::pack_samples<std::complex<char>>(
    const std::complex<char>*, size_t, void*, int, float, gr_complex);
template void difi::pack_samples<std::complex<int8_t>>(
    const std::complex<int8_t>*, size_t, void*, int, float, gr_complex);
template void difi::pack_samples<std::complex<int16_t>>(
    const std::complex<int16_t>*, size_t, void*, int, float, gr_complex);

template size_t
difi::unpack_samples<gr_complex>(const void*, size_t, gr_complex*, size_t, int);
template size_t difi::unpack_samples<std::complex<char>>(
    const void*, size_t, std::complex<char>*, size_t, int);
template size_t difi::unpack_samples<std::complex<int8_t>>(
    const void*, size_t, std::complex<int8_t>*, size_t, int);
template size_t difi::unpack_samples<std::complex<int16_t>>(
    const void*, size_t, std::complex<int16_t>*, size_t, int);

} // namespace gr::difi

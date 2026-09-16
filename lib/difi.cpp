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

    uint32_t w0 = 0;
    uint32_t w1 = 0;
    uint64_t cid = 0;
    uint32_t full = 0;
    uint64_t frac = 0;

    std::memcpy(&w0, in, 4);
    std::memcpy(&w1, in + 4, 4);
    std::memcpy(&cid, in + 8, 8);
    std::memcpy(&full, in + 16, 4);
    std::memcpy(&frac, in + 20, 8);

    w0 = ntohl(w0);
    w1 = ntohl(w1);
    cid = be64toh(cid);
    full = ntohl(full);
    frac = be64toh(frac);

    header h;
    uint8_t type_val = static_cast<uint8_t>(w0 >> 28);
    if (type_val == static_cast<uint8_t>(packet_type::data)) {
        h.type = packet_type::data;
    } else if (type_val == static_cast<uint8_t>(packet_type::context)) {
        h.type = packet_type::context;
    } else if (type_val == static_cast<uint8_t>(packet_type::version)) {
        h.type = packet_type::version;
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
                static_part = STANDARD_CONTEXT_STATIC_BITS;
            } else if (header.type == packet_type::version) {
                static_part = (static_cast<uint32_t>(packet_type::version) << 28) |
                              (STANDARD_CONTEXT_STATIC_BITS & 0x0fffffff);
            } else {
                static_part = STANDARD_DATA_STATIC_BITS;
            }
        } else if (header.type != packet_type::unknown) {
            static_part =
                (static_cast<uint32_t>(header.type) << 28) | (static_part & 0x0fffffff);
        }
        w0 = static_part | (static_cast<uint32_t>(header.pkt_n & 0x0f) << 16) |
             (static_cast<uint32_t>(header.packet_size_words) & 0xffff);
    }

    uint32_t net_w0 = htonl(w0);
    uint32_t net_w1 = htonl(header.stream_id);
    uint64_t net_cid = htobe64(header.class_id);
    uint32_t net_full = htonl(header.full);
    uint64_t net_frac = htobe64(header.frac);

    std::memcpy(out, &net_w0, 4);
    std::memcpy(out + 4, &net_w1, 4);
    std::memcpy(out + 8, &net_cid, 8);
    std::memcpy(out + 16, &net_full, 4);
    std::memcpy(out + 20, &net_frac, 8);
}

namespace {
inline uint32_t unpack_u32(const uint8_t* ptr)
{
    uint32_t val;
    std::memcpy(&val, ptr, 4);
    return ntohl(val);
}

inline uint64_t unpack_u64(const uint8_t* ptr)
{
    uint64_t val;
    std::memcpy(&val, ptr, 8);
    return be64toh(val);
}

inline void pack_u32(uint8_t* ptr, uint32_t val)
{
    uint32_t net = htonl(val);
    std::memcpy(ptr, &net, 4);
}

inline void pack_u64(uint8_t* ptr, uint64_t val)
{
    uint64_t net = htobe64(val);
    std::memcpy(ptr, &net, 8);
}
} // namespace

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

    int idx = 0;
    ctx.class_id = unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.full = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.frac = unpack_u64(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.cif = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
    ctx.ref_point = unpack_u32(&in[CONTEXT_PACKET_OFFSETS[idx++]]);
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
std::vector<uint8_t> difi::pack_context<difi::context_version::standard>(
    const standard_context& ctx, uint8_t pkt_n, uint32_t stream_id, size_t packet_size)
{
    std::vector<uint8_t> out(packet_size, 0);

    uint32_t header = STANDARD_CONTEXT_STATIC_BITS |
                      (static_cast<uint32_t>(pkt_n & 0x0f) << 16) |
                      static_cast<uint32_t>(packet_size / 4);

    pack_u32(&out[0], header);
    pack_u32(&out[4], stream_id);

    int idx = 0;
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.class_id);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.full);
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.frac);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.cif != 0 ? ctx.cif : 0xfbb98000U);
    pack_u32(&out[CONTEXT_PACKET_OFFSETS[idx++]],
             ctx.ref_point != 0 ? ctx.ref_point : 0x64U);
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
                 : static_cast<uint32_t>(DEFAULT_STATE_AND_EVENTS));
    pack_u64(&out[CONTEXT_PACKET_OFFSETS[idx++]], ctx.payload_format);

    return out;
}

template <>
std::vector<uint8_t> difi::pack_context<difi::context_version::alternative>(
    const alternative_context& ctx, uint8_t pkt_n, uint32_t stream_id, size_t packet_size)
{
    std::vector<uint8_t> out(packet_size, 0);

    uint32_t header = ALT_CONTEXT_STATIC_BITS |
                      (static_cast<uint32_t>(pkt_n & 0x0f) << 16) |
                      static_cast<uint32_t>(packet_size / 4);

    pack_u32(&out[0], header);
    pack_u32(&out[4], stream_id);

    int idx = 0;
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]], ctx.class_id);
    pack_u32(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]],
             ctx.cif != 0 ? ctx.cif : 966885376U);
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
                 : static_cast<uint32_t>(DEFAULT_STATE_AND_EVENTS));
    pack_u64(&out[CONTEXT_PACKET_ALT_OFFSETS[idx++]], ctx.payload_format);

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
difi::standard_context difi::make_default_context<difi::context_version::standard>(
    double samp_rate, int depth, uint32_t stream_id, uint32_t full, uint64_t frac)
{
    standard_context ctx;
    ctx.class_id = (STANDARD_OUI << 32) ^ 1;
    ctx.full = full;
    ctx.frac = frac;
    ctx.cif = 0xfbb98000U;
    ctx.ref_point = 0x64;
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
    ctx.state_and_event_indicators = static_cast<uint32_t>(DEFAULT_STATE_AND_EVENTS);
    ctx.payload_format =
        (depth == 8) ? EIGHT_BIT_SIGNED_CART_LINK_EFF : SIXTEEN_BIT_SIGNED_CART_LINK_EFF;
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
    ctx.class_id = (ALT_OUI << 32) ^ 1;
    ctx.cif = 966885376U;
    ctx.bandwidth = samp_rate * 0.8;
    ctx.if_ref_freq = 0.0;
    ctx.rf_ref_freq = 0.0;
    ctx.if_band_offset = 0.0;
    ctx.sample_rate = samp_rate;
    ctx.state_and_event_indicators = static_cast<uint32_t>(DEFAULT_STATE_AND_EVENTS);
    ctx.payload_format =
        (depth == 8) ? EIGHT_BIT_SIGNED_CART_LINK_EFF : SIXTEEN_BIT_SIGNED_CART_LINK_EFF;
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
        ctx.class_id = (ALT_OUI << 32) ^ 1;
        ctx.full = full;
        ctx.frac = frac;
        ctx.cif = 966885376U;
        ctx.ref_point = 0x64;
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
void difi::pack_samples(const T* in,
                        size_t num_samples,
                        void* payload,
                        int depth,
                        int scaling_mode,
                        float gain,
                        gr_complex offset)
{
    if (depth == 16) {
        int16_t* out16 = static_cast<int16_t*>(payload);
        for (size_t i = 0; i < num_samples; ++i) {
            gr_complex val(in[i].real(), in[i].imag());
            if (scaling_mode > 0) {
                val = (val + offset) * gain;
            }
            out16[2 * i] = static_cast<int16_t>(val.real());
            out16[2 * i + 1] = static_cast<int16_t>(val.imag());
        }
        swap_endian_16(reinterpret_cast<uint16_t*>(out16), num_samples * 2);
    } else {
        int8_t* out8 = static_cast<int8_t*>(payload);
        for (size_t i = 0; i < num_samples; ++i) {
            gr_complex val(in[i].real(), in[i].imag());
            if (scaling_mode > 0) {
                val = (val + offset) * gain;
            }
            out8[2 * i] = static_cast<int8_t>(val.real());
            out8[2 * i + 1] = static_cast<int8_t>(val.imag());
        }
    }
}

template <sample_type T>
size_t difi::unpack_samples(
    const void* payload, size_t payload_bytes, T* out, size_t max_samples, int depth)
{
    size_t bytes_per_sample = (depth == 16) ? 4 : 2;
    size_t available = payload_bytes / bytes_per_sample;
    size_t count = std::min(available, max_samples);

    if (depth == 16) {
        const int16_t* in16 = static_cast<const int16_t*>(payload);
        std::vector<int16_t> buf(count * 2);
        std::memcpy(buf.data(), in16, count * 4);
        swap_endian_16(reinterpret_cast<uint16_t*>(buf.data()), count * 2);
        for (size_t i = 0; i < count; ++i) {
            out[i] = T(buf[2 * i], buf[2 * i + 1]);
        }
    } else {
        const int8_t* in8 = static_cast<const int8_t*>(payload);
        for (size_t i = 0; i < count; ++i) {
            out[i] = T(in8[2 * i], in8[2 * i + 1]);
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
            dict, pmt::intern("reference_point"), pmt::from_long(ctx.ref_point));
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

// Explicit template instantiations
template void difi::pack_samples<gr_complex>(
    const gr_complex*, size_t, void*, int, int, float, gr_complex);

template void difi::pack_samples<std::complex<char>>(
    const std::complex<char>*, size_t, void*, int, int, float, gr_complex);

template size_t
difi::unpack_samples<gr_complex>(const void*, size_t, gr_complex*, size_t, int);

template size_t difi::unpack_samples<std::complex<char>>(
    const void*, size_t, std::complex<char>*, size_t, int);

} // namespace gr::difi

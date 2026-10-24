/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#define BOOST_TEST_MODULE qa_difi

#include <difi/difi.hpp>
#include <difi/difi_sink_cpp.h>
#include <boost/test/unit_test.hpp>
#include <cmath>
#include <span>
#include <vector>

using namespace gr::difi;

BOOST_AUTO_TEST_CASE(t_byte_order_pack_unpack)
{
    uint8_t buf32[4] = {};
    difi::pack_u32(buf32, 0x12345678);
    BOOST_CHECK_EQUAL(buf32[0], 0x12);
    BOOST_CHECK_EQUAL(buf32[1], 0x34);
    BOOST_CHECK_EQUAL(buf32[2], 0x56);
    BOOST_CHECK_EQUAL(buf32[3], 0x78);
    BOOST_CHECK_EQUAL(difi::unpack_u32(buf32), 0x12345678U);

    uint8_t buf64[8] = {};
    difi::pack_u64(buf64, 0x0123456789abcdefULL);
    BOOST_CHECK_EQUAL(buf64[0], 0x01);
    BOOST_CHECK_EQUAL(buf64[1], 0x23);
    BOOST_CHECK_EQUAL(buf64[2], 0x45);
    BOOST_CHECK_EQUAL(buf64[3], 0x67);
    BOOST_CHECK_EQUAL(buf64[4], 0x89);
    BOOST_CHECK_EQUAL(buf64[5], 0xab);
    BOOST_CHECK_EQUAL(buf64[6], 0xcd);
    BOOST_CHECK_EQUAL(buf64[7], 0xef);
    BOOST_CHECK_EQUAL(difi::unpack_u64(buf64), 0x0123456789abcdefULL);

    int8_t sbuf32[4] = {};
    difi::pack_u32(sbuf32, 0xaabbccddU);
    BOOST_CHECK_EQUAL(difi::unpack_u32(sbuf32), 0xaabbccddU);
}

BOOST_AUTO_TEST_CASE(t_header_parse_and_pack)
{
    difi_header h;
    h.type = packet_type::data;
    h.pkt_n = 9;
    h.packet_size_words = 100;
    h.stream_id = 0x12345678;
    h.class_id = 0x6a621e0000000000ULL;
    h.full = 1726390000U;
    h.frac = 500000000000ULL; // 0.5 s in picoseconds
    h.static_bits = difi::STANDARD_DATA_STATIC_BITS;

    uint8_t buffer[difi::HEADER_SIZE] = {};
    difi::pack_header(buffer, h);

    difi_header parsed = difi::parse_header(buffer, sizeof(buffer));

    BOOST_CHECK_EQUAL(static_cast<int>(parsed.type), static_cast<int>(packet_type::data));
    BOOST_CHECK_EQUAL(parsed.pkt_n, 9);
    BOOST_CHECK_EQUAL(parsed.packet_size_words, 100);
    BOOST_CHECK_EQUAL(parsed.packet_size_bytes, 400);
    BOOST_CHECK_EQUAL(parsed.stream_id, 0x12345678);
    BOOST_CHECK_EQUAL(parsed.class_id, 0x6a621e0000000000ULL);
    BOOST_CHECK_EQUAL(parsed.full, 1726390000U);
    BOOST_CHECK_EQUAL(parsed.frac, 500000000000ULL);
    BOOST_CHECK_EQUAL(parsed.static_bits, difi::STANDARD_DATA_STATIC_BITS);

    // Test span-based pack_header and parse_header
    uint8_t span_buffer[difi::HEADER_SIZE] = {};
    std::span<uint8_t> out_span(span_buffer);
    h.type = packet_type::version;
    difi::pack_header(out_span, h);
    difi_header span_parsed = difi::parse_header(std::span<const uint8_t>(span_buffer));
    BOOST_CHECK_EQUAL(static_cast<int>(span_parsed.type),
                      static_cast<int>(packet_type::version));

    // Parsing buffer smaller than 28 bytes throws
    BOOST_CHECK_THROW(difi::parse_header(buffer, 20), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_vita_fixed_point_conversions)
{
    // Double precision conversions (20 fractional bits)
    double test_rates[] = { 1000000.0, 20480000.0, 100000000.0, 7500000000.0 };
    for (double rate : test_rates) {
        uint64_t bits = difi::double_to_vita_fixed(rate);
        double recovered = difi::vita_fixed_to_double(bits);
        BOOST_CHECK_CLOSE(rate, recovered, 0.0001);
    }

    // Signed double precision
    double offset = -200000.0;
    int64_t signed_bits = static_cast<int64_t>(offset * (1ULL << 20));
    double recovered_offset = difi::vita_fixed_to_double(signed_bits);
    BOOST_CHECK_CLOSE(offset, recovered_offset, 0.0001);

    // Float precision conversions (7 fractional bits)
    float test_gains[] = { 14.2f, -1.3f, 20.0f, -10.5f };
    for (float gain : test_gains) {
        int16_t bits = difi::float_to_vita_fixed(gain);
        float recovered = difi::vita_fixed_to_float(bits);
        BOOST_CHECK_CLOSE(gain, recovered, 0.5); // 1/128 resolution ~ 0.008
    }
}

BOOST_AUTO_TEST_CASE(t_simd_endian_swap_16)
{
    // Test byte swapping with VOLK SIMD kernel across various sizes
    const size_t test_sizes[] = { 1, 2, 7, 16, 32, 63, 128, 512, 1024 };

    for (size_t n : test_sizes) {
        std::vector<uint16_t> data(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] = static_cast<uint16_t>(0x1234 + i);
        }

        std::vector<uint16_t> original = data;
        difi::swap_endian_16(data.data(), n);

        if constexpr (std::endian::native == std::endian::little) {
            for (size_t i = 0; i < n; ++i) {
                uint16_t expected = static_cast<uint16_t>(((original[i] & 0xff) << 8) |
                                                          ((original[i] >> 8) & 0xff));
                BOOST_CHECK_EQUAL(data[i], expected);
            }
        }

        // Swapping again with std::span should restore original data
        difi::swap_endian_16(std::span<uint16_t>(data));
        for (size_t i = 0; i < n; ++i) {
            BOOST_CHECK_EQUAL(data[i], original[i]);
        }
    }
}

BOOST_AUTO_TEST_CASE(t_context_standard_108_parse_and_pack)
{
    double samp_rate = 1000000.0;
    uint32_t stream_id = 42;
    uint32_t full = 1000;
    uint64_t frac = 250000000000ULL;

    standard_context ctx = difi::make_default_context<context_version::standard>(
        samp_rate, 16, stream_id, full, frac);

    BOOST_CHECK_EQUAL(ctx.packet_size, difi::STANDARD_CONTEXT_SIZE);
    BOOST_CHECK_EQUAL(ctx.raw.size(), difi::STANDARD_CONTEXT_SIZE);

    // Parse the raw bytes using static template
    standard_context parsed =
        difi::parse_context<context_version::standard>(ctx.raw.data(), ctx.raw.size());

    BOOST_CHECK_EQUAL(parsed.class_id, ctx.class_id);
    BOOST_CHECK_EQUAL(parsed.full, full);
    BOOST_CHECK_EQUAL(parsed.frac, frac);
    BOOST_CHECK_CLOSE(parsed.bandwidth, samp_rate * 0.8, 0.01);
    BOOST_CHECK_CLOSE(parsed.sample_rate, samp_rate, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_ref_freq, 100000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_ref_freq, 7500000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.ref_level, 20.0f, 0.1);
    BOOST_CHECK_CLOSE(parsed.rf_gain, 14.2f, 0.1);
    BOOST_CHECK_CLOSE(parsed.if_gain, -1.3f, 0.5);
    BOOST_CHECK_EQUAL(parsed.state_and_event_indicators, difi::DEFAULT_STATE_AND_EVENTS);
    BOOST_CHECK_EQUAL(parsed.payload_format, difi::SIXTEEN_BIT_SIGNED_CART_LINK_EFF);

    // Test span-based parse_context
    std::span<const uint8_t> raw_span(reinterpret_cast<const uint8_t*>(ctx.raw.data()),
                                      ctx.raw.size());
    standard_context span_parsed =
        difi::parse_context<context_version::standard>(raw_span);
    BOOST_CHECK_EQUAL(span_parsed.class_id, ctx.class_id);

    // Test parse throws when buffer too small
    BOOST_CHECK_THROW(difi::parse_context<context_version::standard>(
                          ctx.raw.data(), difi::STANDARD_CONTEXT_SIZE - 1),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_context_alt_72_parse_and_pack)
{
    double samp_rate = 2000000.0;
    uint32_t stream_id = 99;

    alternative_context ctx =
        difi::make_default_context<context_version::alternative>(samp_rate, 8, stream_id);

    BOOST_CHECK_EQUAL(ctx.packet_size, difi::ALT_CONTEXT_SIZE);
    BOOST_CHECK_EQUAL(ctx.raw.size(), difi::ALT_CONTEXT_SIZE);

    // Parse the raw bytes using static template
    alternative_context parsed =
        difi::parse_context<context_version::alternative>(ctx.raw.data(), ctx.raw.size());

    BOOST_CHECK_EQUAL(parsed.class_id, ctx.class_id);
    BOOST_CHECK_CLOSE(parsed.bandwidth, samp_rate * 0.8, 0.01);
    BOOST_CHECK_CLOSE(parsed.sample_rate, samp_rate, 0.01);
    BOOST_CHECK_EQUAL(parsed.payload_format, difi::EIGHT_BIT_SIGNED_CART_LINK_EFF);

    // Test span-based parse_context
    std::span<const uint8_t> raw_span(reinterpret_cast<const uint8_t*>(ctx.raw.data()),
                                      ctx.raw.size());
    alternative_context span_parsed =
        difi::parse_context<context_version::alternative>(raw_span);
    BOOST_CHECK_EQUAL(span_parsed.class_id, ctx.class_id);

    // Test parse throws when buffer too small
    BOOST_CHECK_THROW(difi::parse_context<context_version::alternative>(
                          ctx.raw.data(), difi::ALT_CONTEXT_SIZE - 1),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(t_context_runtime_dispatch_and_variant)
{
    double samp_rate = 1000000.0;
    uint32_t stream_id = 77;

    // 1. Standard context runtime dispatch (108 bytes)
    standard_context std_ctx =
        difi::make_default_context<context_version::standard>(samp_rate, 16, stream_id);
    context_variant var_std = difi::parse_context(std_ctx.raw.data(), std_ctx.raw.size());

    BOOST_CHECK(std::holds_alternative<standard_context>(var_std));
    BOOST_CHECK_EQUAL(std::get<standard_context>(var_std).class_id, std_ctx.class_id);

    // 2. Alternative context runtime dispatch (72 bytes)
    alternative_context alt_ctx =
        difi::make_default_context<context_version::alternative>(samp_rate, 8, stream_id);
    context_variant var_alt = difi::parse_context(alt_ctx.raw.data(), alt_ctx.raw.size());

    BOOST_CHECK(std::holds_alternative<alternative_context>(var_alt));
    BOOST_CHECK_EQUAL(std::get<alternative_context>(var_alt).class_id, alt_ctx.class_id);

    // 3. Span-based runtime dispatch
    std::span<const uint8_t> alt_span(
        reinterpret_cast<const uint8_t*>(alt_ctx.raw.data()), alt_ctx.raw.size());
    context_variant var_span = difi::parse_context(alt_span);
    BOOST_CHECK(std::holds_alternative<alternative_context>(var_span));

    // 4. Invalid size throws runtime_error
    uint8_t invalid_buf[50] = {};
    BOOST_CHECK_THROW(difi::parse_context(invalid_buf, sizeof(invalid_buf)),
                      std::runtime_error);
    uint8_t invalid_buf2[80] = {};
    BOOST_CHECK_THROW(difi::parse_context(invalid_buf2, sizeof(invalid_buf2)),
                      std::runtime_error);

    // 5. Pack from variant
    std::vector<uint8_t> packed_std = difi::pack_context(var_std, 1, stream_id);
    BOOST_CHECK_EQUAL(packed_std.size(), difi::STANDARD_CONTEXT_SIZE);

    std::vector<uint8_t> packed_alt = difi::pack_context(var_alt, 2, stream_id);
    BOOST_CHECK_EQUAL(packed_alt.size(), difi::ALT_CONTEXT_SIZE);
}

BOOST_AUTO_TEST_CASE(t_context_span_pack)
{
    double samp_rate = 1000000.0;
    uint32_t stream_id = 77;

    // 1. Standard context span packing
    standard_context std_ctx =
        difi::make_default_context<context_version::standard>(samp_rate, 16, stream_id, 1234, 5678);
    std::vector<uint8_t> span_buf_std(difi::STANDARD_CONTEXT_SIZE, 0);
    difi::pack_context(std_ctx, 5, stream_id, std::span<uint8_t>(span_buf_std));

    std::vector<uint8_t> vec_std = difi::pack_context(std_ctx, 5, stream_id);
    BOOST_CHECK_EQUAL_COLLECTIONS(
        span_buf_std.begin(), span_buf_std.end(), vec_std.begin(), vec_std.end());

    // 2. Alternative context span packing
    alternative_context alt_ctx =
        difi::make_default_context<context_version::alternative>(samp_rate, 8, stream_id);
    std::vector<uint8_t> span_buf_alt(difi::ALT_CONTEXT_SIZE, 0);
    difi::pack_context(alt_ctx, 6, stream_id, std::span<uint8_t>(span_buf_alt));

    std::vector<uint8_t> vec_alt = difi::pack_context(alt_ctx, 6, stream_id);
    BOOST_CHECK_EQUAL_COLLECTIONS(
        span_buf_alt.begin(), span_buf_alt.end(), vec_alt.begin(), vec_alt.end());

    // 3. Variant span packing
    context_variant var_std = std_ctx;
    std::fill(span_buf_std.begin(), span_buf_std.end(), 0);
    difi::pack_context(var_std, 5, stream_id, std::span<uint8_t>(span_buf_std));
    BOOST_CHECK_EQUAL_COLLECTIONS(
        span_buf_std.begin(), span_buf_std.end(), vec_std.begin(), vec_std.end());

    // 4. Buffer too small throws std::invalid_argument
    std::vector<uint8_t> small_std(difi::STANDARD_CONTEXT_SIZE - 1);
    BOOST_CHECK_THROW(difi::pack_context(std_ctx, 1, stream_id, std::span<uint8_t>(small_std)),
                      std::invalid_argument);

    std::vector<uint8_t> small_alt(difi::ALT_CONTEXT_SIZE - 1);
    BOOST_CHECK_THROW(difi::pack_context(alt_ctx, 1, stream_id, std::span<uint8_t>(small_alt)),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(t_sample_packing_and_unpacking_16bit)
{
    const size_t N = 128;
    std::vector<gr_complex> samples_in(N);
    for (size_t i = 0; i < N; ++i) {
        samples_in[i] = gr_complex(static_cast<float>(i * 10),
                                   static_cast<float>(-static_cast<int>(i) * 10));
    }

    std::vector<uint8_t> payload(N * 4);
    difi::pack_samples(samples_in.data(), N, payload.data(), 16);

    std::vector<gr_complex> samples_out(N);
    size_t unpacked =
        difi::unpack_samples(payload.data(), payload.size(), samples_out.data(), N, 16);

    BOOST_CHECK_EQUAL(unpacked, N);
    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(samples_out[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(samples_out[i].imag(), samples_in[i].imag());
    }

    // Test span overload
    std::vector<uint8_t> span_payload(N * 4);
    difi::pack_samples(
        std::span<const gr_complex>(samples_in), std::span<uint8_t>(span_payload), 16);
    std::vector<gr_complex> span_out(N);
    size_t span_unpacked = difi::unpack_samples(
        std::span<const uint8_t>(span_payload), std::span<gr_complex>(span_out), 16);
    BOOST_CHECK_EQUAL(span_unpacked, N);
    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(span_out[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(span_out[i].imag(), samples_in[i].imag());
    }
}

BOOST_AUTO_TEST_CASE(t_sample_packing_and_unpacking_8bit)
{
    const size_t N = 64;
    std::vector<gr_complex> samples_in(N);
    for (size_t i = 0; i < N; ++i) {
        samples_in[i] = gr_complex(static_cast<float>(static_cast<int>(i) - 32),
                                   static_cast<float>(32 - static_cast<int>(i)));
    }

    std::vector<uint8_t> payload(N * 2);
    difi::pack_samples(samples_in.data(), N, payload.data(), 8);

    std::vector<gr_complex> samples_out(N);
    size_t unpacked =
        difi::unpack_samples(payload.data(), payload.size(), samples_out.data(), N, 8);

    BOOST_CHECK_EQUAL(unpacked, N);
    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(samples_out[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(samples_out[i].imag(), samples_in[i].imag());
    }
}

BOOST_AUTO_TEST_CASE(t_sample_packing_scaling_and_offset)
{
    const size_t N = 32;
    std::vector<gr_complex> samples_in(N);
    for (size_t i = 0; i < N; ++i) {
        samples_in[i] =
            gr_complex(static_cast<float>(i), static_cast<float>(-static_cast<int>(i)));
    }

    float gain = 2.0f;
    gr_complex offset(10.0f, -5.0f);

    // 16-bit depth with scaling (different real/imag offset, tests volk_32fc_x2_add_32fc)
    std::vector<uint8_t> payload16(N * 4);
    difi::pack_samples(samples_in.data(), N, payload16.data(), 16, gain, offset);

    std::vector<gr_complex> samples_out16(N);
    size_t unpacked16 = difi::unpack_samples(
        payload16.data(), payload16.size(), samples_out16.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked16, N);

    for (size_t i = 0; i < N; ++i) {
        float expected_re = (samples_in[i].real() + offset.real()) * gain;
        float expected_im = (samples_in[i].imag() + offset.imag()) * gain;
        BOOST_CHECK_CLOSE(samples_out16[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(samples_out16[i].imag(), expected_im, 0.01);
    }

    // 16-bit depth span overload with scaling
    std::vector<uint8_t> span_payload16(N * 4);
    difi::pack_samples(
        std::span<const gr_complex>(samples_in), std::span<uint8_t>(span_payload16), 16, gain, offset);
    std::vector<gr_complex> span_out16(N);
    size_t span_unpacked16 = difi::unpack_samples(
        span_payload16.data(), span_payload16.size(), span_out16.data(), N, 16);
    BOOST_CHECK_EQUAL(span_unpacked16, N);
    for (size_t i = 0; i < N; ++i) {
        float expected_re = (samples_in[i].real() + offset.real()) * gain;
        float expected_im = (samples_in[i].imag() + offset.imag()) * gain;
        BOOST_CHECK_CLOSE(span_out16[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(span_out16[i].imag(), expected_im, 0.01);
    }

    // 8-bit depth with scaling
    std::vector<uint8_t> payload8(N * 2);
    difi::pack_samples(samples_in.data(), N, payload8.data(), 8, gain, offset);

    std::vector<gr_complex> samples_out8(N);
    size_t unpacked8 =
        difi::unpack_samples(payload8.data(), payload8.size(), samples_out8.data(), N, 8);
    BOOST_CHECK_EQUAL(unpacked8, N);

    for (size_t i = 0; i < N; ++i) {
        float expected_re = (samples_in[i].real() + offset.real()) * gain;
        float expected_im = (samples_in[i].imag() + offset.imag()) * gain;
        BOOST_CHECK_CLOSE(samples_out8[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(samples_out8[i].imag(), expected_im, 0.01);
    }

    // Test equal real and imag offset
    gr_complex equal_offset(4.0f, 4.0f);
    std::vector<uint8_t> payload_eq(N * 4);
    difi::pack_samples(samples_in.data(), N, payload_eq.data(), 16, gain, equal_offset);

    std::vector<gr_complex> samples_out_eq(N);
    size_t unpacked_eq = difi::unpack_samples(
        payload_eq.data(), payload_eq.size(), samples_out_eq.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked_eq, N);
    for (size_t i = 0; i < N; ++i) {
        float expected_re = (samples_in[i].real() + equal_offset.real()) * gain;
        float expected_im = (samples_in[i].imag() + equal_offset.imag()) * gain;
        BOOST_CHECK_CLOSE(samples_out_eq[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(samples_out_eq[i].imag(), expected_im, 0.01);
    }

    // Test gain only with default offset
    std::vector<uint8_t> payload_gain_only(N * 4);
    difi::pack_samples(samples_in.data(), N, payload_gain_only.data(), 16, gain);

    std::vector<gr_complex> samples_out_gain_only(N);
    size_t unpacked_gain_only = difi::unpack_samples(
        payload_gain_only.data(), payload_gain_only.size(), samples_out_gain_only.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked_gain_only, N);
    for (size_t i = 0; i < N; ++i) {
        float expected_re = samples_in[i].real() * gain;
        float expected_im = samples_in[i].imag() * gain;
        BOOST_CHECK_CLOSE(samples_out_gain_only[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(samples_out_gain_only[i].imag(), expected_im, 0.01);
    }
}

BOOST_AUTO_TEST_CASE(t_sample_packing_and_unpacking_sc8)
{
    const size_t N = 64;
    std::vector<std::complex<char>> samples_in(N);
    for (size_t i = 0; i < N; ++i) {
        samples_in[i] =
            std::complex<char>(static_cast<char>(i - 32), static_cast<char>(32 - i));
    }

    // 8-bit wire format with sc8 (no scaling)
    std::vector<uint8_t> payload8(N * 2);
    difi::pack_samples(samples_in.data(), N, payload8.data(), 8);

    std::vector<std::complex<char>> samples_out8(N);
    size_t unpacked8 =
        difi::unpack_samples(payload8.data(), payload8.size(), samples_out8.data(), N, 8);
    BOOST_CHECK_EQUAL(unpacked8, N);

    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(samples_out8[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(samples_out8[i].imag(), samples_in[i].imag());
    }

    // 16-bit wire format with sc8 (no scaling)
    std::vector<uint8_t> payload16(N * 4);
    difi::pack_samples(samples_in.data(), N, payload16.data(), 16);

    std::vector<std::complex<char>> samples_out16(N);
    size_t unpacked16 = difi::unpack_samples(
        payload16.data(), payload16.size(), samples_out16.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked16, N);

    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(samples_out16[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(samples_out16[i].imag(), samples_in[i].imag());
    }

    // 16-bit wire format with sc8 and scaling
    float gain = 2.0f;
    gr_complex offset(5.0f, -3.0f);
    std::vector<uint8_t> payload16_scaled(N * 4);
    difi::pack_samples(samples_in.data(), N, payload16_scaled.data(), 16, gain, offset);

    std::vector<gr_complex> samples_out16_scaled(N);
    size_t unpacked16_scaled = difi::unpack_samples(
        payload16_scaled.data(), payload16_scaled.size(), samples_out16_scaled.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked16_scaled, N);

    for (size_t i = 0; i < N; ++i) {
        float expected_re = (static_cast<float>(samples_in[i].real()) + offset.real()) * gain;
        float expected_im = (static_cast<float>(samples_in[i].imag()) + offset.imag()) * gain;
        BOOST_CHECK_CLOSE(samples_out16_scaled[i].real(), expected_re, 0.01);
        BOOST_CHECK_CLOSE(samples_out16_scaled[i].imag(), expected_im, 0.01);
    }
}

BOOST_AUTO_TEST_CASE(t_sample_packing_complex_int16)
{
    const size_t N = 8;
    std::vector<std::complex<int16_t>> samples_in(N);
    for (size_t i = 0; i < N; ++i) {
        samples_in[i] = std::complex<int16_t>(
            static_cast<int16_t>(i * 1000 - 4000),
            static_cast<int16_t>(4000 - i * 1000));
    }

    // 16-bit wire format (network byte order)
    std::vector<uint8_t> payload16(N * 4);
    difi::pack_samples(samples_in.data(), N, payload16.data(), 16);

    // Verify first sample endianness (big-endian)
    uint16_t expected_re_be = htons(static_cast<uint16_t>(samples_in[0].real()));
    uint16_t actual_re_be;
    std::memcpy(&actual_re_be, payload16.data(), sizeof(uint16_t));
    BOOST_CHECK_EQUAL(actual_re_be, expected_re_be);

    // Unpack back to complex<int16_t>
    std::vector<std::complex<int16_t>> samples_out16(N);
    size_t unpacked16 = difi::unpack_samples(
        payload16.data(), payload16.size(), samples_out16.data(), N, 16);
    BOOST_CHECK_EQUAL(unpacked16, N);

    for (size_t i = 0; i < N; ++i) {
        BOOST_CHECK_EQUAL(samples_out16[i].real(), samples_in[i].real());
        BOOST_CHECK_EQUAL(samples_out16[i].imag(), samples_in[i].imag());
    }

    // Span-based overload
    std::vector<uint8_t> span_payload16(N * 4);
    difi::pack_samples(std::span<const std::complex<int16_t>>(samples_in),
                       std::span<uint8_t>(span_payload16), 16);
    BOOST_CHECK_EQUAL_COLLECTIONS(payload16.begin(), payload16.end(),
                                  span_payload16.begin(), span_payload16.end());
}

BOOST_AUTO_TEST_CASE(t_timestamp_advance)
{
    uint32_t full = 100;
    uint64_t frac = 900000000000ULL; // 0.9s in picoseconds
    double rate = 1000.0;            // 1000 samples per sec -> 1 ms per sample

    // 200 samples at 1000 Hz = 0.2 seconds = 200,000,000,000 picoseconds
    // Expected new time: 100s + 0.9s + 0.2s = 101s + 0.1s (100,000,000,000 picoseconds)
    auto [new_full, new_frac] = difi::advance_timestamp(full, frac, 200, rate);

    BOOST_CHECK_EQUAL(new_full, 101);
    BOOST_CHECK_EQUAL(new_frac, 100000000000ULL);
}

BOOST_AUTO_TEST_CASE(t_pmt_conversions)
{
    difi_header h;
    h.raw_header = 0x49e0001b;
    h.stream_id = 7;

    standard_context ctx = difi::make_default_context<context_version::standard>(
        1000000.0, 16, 7, 50, 123456ULL);

    pmt::pmt_t dict = difi::context_to_pmt(h, ctx);
    BOOST_CHECK(pmt::is_dict(dict));

    standard_context parsed_ctx = difi::pmt_to_context(dict);
    BOOST_CHECK_EQUAL(parsed_ctx.full, 50);
    BOOST_CHECK_EQUAL(parsed_ctx.frac, 123456ULL);
    BOOST_CHECK_CLOSE(parsed_ctx.sample_rate, 1000000.0, 0.01);

    // Alternative context PMT conversion
    alternative_context alt_ctx =
        difi::make_default_context<context_version::alternative>(2000000.0, 8, 7);
    pmt::pmt_t alt_dict = difi::context_to_pmt(h, alt_ctx);
    BOOST_CHECK(pmt::is_dict(alt_dict));

    alternative_context parsed_alt_ctx =
        difi::pmt_to_context<context_version::alternative>(alt_dict);
    BOOST_CHECK_CLOSE(parsed_alt_ctx.sample_rate, 2000000.0, 0.01);

    // PMT packet metadata dict
    pmt::pmt_t pkt_dict = difi::make_pkt_n_dict(3, 1400, 10, 20);
    BOOST_CHECK(pmt::is_dict(pkt_dict));
    BOOST_CHECK_EQUAL(
        pmt::to_uint64(pmt::dict_ref(pkt_dict, pmt::intern("pck_n"), pmt::get_PMT_NIL())),
        3);
    BOOST_CHECK_EQUAL(pmt::to_uint64(pmt::dict_ref(
                          pkt_dict, pmt::intern("data_len"), pmt::get_PMT_NIL())),
                      1400);
}

BOOST_AUTO_TEST_CASE(t_difi_sink_template_instantiations)
{
    // Default: standard, udp_client, 16-bit
    auto sink_default = difi_sink_cpp<>::make(
        "127.0.0.1", 5000, 1000000, 512);
    BOOST_REQUIRE(sink_default != nullptr);
    auto ctx_default = sink_default->context();
    BOOST_CHECK_EQUAL(std::get<standard_context>(ctx_default).packet_size, 108);

    // Alternative context, UDP, 16-bit
    auto sink_alt_udp = difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_16>::make(
        "127.0.0.1", 5001, 2000000, 256);
    BOOST_REQUIRE(sink_alt_udp != nullptr);
    auto ctx_alt = sink_alt_udp->context();
    BOOST_CHECK_EQUAL(std::get<alternative_context>(ctx_alt).packet_size, 72);

    // Standard context, TCP, 8-bit
    auto sink_std_tcp_8 = difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_8>::make(
        "127.0.0.1", 5002, 1000000, 512);
    BOOST_REQUIRE(sink_std_tcp_8 != nullptr);

    // Standard context, UDP, 8-bit
    auto sink_sc8 = difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_8>::make(
        "127.0.0.1", 5003, 1000000, 512);
    BOOST_REQUIRE(sink_sc8 != nullptr);

    // Streaming mode (default length_tag_name is empty)
    BOOST_CHECK_EQUAL(sink_default->burst_mode(), false);
    BOOST_CHECK_EQUAL(sink_default->length_tag_name(), "");

    // Burst mode with explicit length_tag_name
    auto sink_burst = difi_sink_cpp<>::make(
        "127.0.0.1", 5004, 1000000, 512, 0, 100, 0, 0, "packet_len", 5000);
    BOOST_REQUIRE(sink_burst != nullptr);
    BOOST_CHECK_EQUAL(sink_burst->burst_mode(), true);
    BOOST_CHECK_EQUAL(sink_burst->length_tag_name(), "packet_len");
    BOOST_CHECK_EQUAL(sink_burst->lead_time_us(), 5000);

    sink_burst->set_length_tag_name("");
    BOOST_CHECK_EQUAL(sink_burst->burst_mode(), false);
    BOOST_CHECK_EQUAL(sink_burst->length_tag_name(), "");

    // Test RF options in constructor and getters/setters
    auto sink_rf = difi_sink_cpp<>::make(
        "127.0.0.1", 5005, 1000000, 512, 0, 100, 0, 0, "", 10000,
        1420000000.0, 70000000.0, 8000000.0, 20.0f, -2.0f, 15.0f, 5000.0);
    BOOST_REQUIRE(sink_rf != nullptr);
    BOOST_CHECK_CLOSE(sink_rf->rf_freq(), 1420000000.0, 0.01);
    BOOST_CHECK_CLOSE(sink_rf->center_freq(), 1420000000.0, 0.01);
    BOOST_CHECK_CLOSE(sink_rf->if_freq(), 70000000.0, 0.01);
    BOOST_CHECK_CLOSE(sink_rf->bandwidth(), 8000000.0, 0.01);
    BOOST_CHECK_CLOSE(sink_rf->rf_gain(), 20.0f, 0.1);
    BOOST_CHECK_CLOSE(sink_rf->gain(), 20.0f, 0.1);
    BOOST_CHECK_CLOSE(sink_rf->if_gain(), -2.0f, 0.1);
    BOOST_CHECK_CLOSE(sink_rf->ref_level(), 15.0f, 0.1);
    BOOST_CHECK_CLOSE(sink_rf->if_band_offset(), 5000.0, 0.01);

    // Dynamic runtime updates
    sink_rf->set_center_freq(2400000000.0);
    BOOST_CHECK_CLOSE(sink_rf->center_freq(), 2400000000.0, 0.01);
    sink_rf->set_gain(30.0f);
    BOOST_CHECK_CLOSE(sink_rf->gain(), 30.0f, 0.1);
    sink_rf->set_bandwidth(10000000.0);
    BOOST_CHECK_CLOSE(sink_rf->bandwidth(), 10000000.0, 0.01);

    auto ctx_rf = std::get<standard_context>(sink_rf->context());
    BOOST_CHECK_CLOSE(ctx_rf.rf_ref_freq, 2400000000.0, 0.01);
    BOOST_CHECK_CLOSE(ctx_rf.rf_gain, 30.0f, 0.1);
    BOOST_CHECK_CLOSE(ctx_rf.bandwidth, 10000000.0, 0.01);

    // Time source (TSI) default and runtime setter
    BOOST_CHECK(sink_rf->time_source() == difi::tsi_type::posix);
    BOOST_CHECK(sink_rf->tsi() == difi::tsi_type::posix);
    BOOST_CHECK(ctx_rf.tsi == difi::tsi_type::posix);

    sink_rf->set_time_source(difi::tsi_type::utc);
    BOOST_CHECK(sink_rf->time_source() == difi::tsi_type::utc);
    BOOST_CHECK(sink_rf->tsi() == difi::tsi_type::utc);
    auto ctx_utc = std::get<standard_context>(sink_rf->context());
    BOOST_CHECK(ctx_utc.tsi == difi::tsi_type::utc);
}

BOOST_AUTO_TEST_CASE(t_make_context_standard_defaults)
{
    difi::context_config cfg{
        .sample_rate = 2000000.0,
        .depth = 16,
        .stream_id = 10,
    };

    standard_context ctx = difi::make_context<context_version::standard>(cfg);

    BOOST_CHECK_EQUAL(ctx.packet_size, difi::STANDARD_CONTEXT_SIZE);
    BOOST_CHECK_EQUAL(ctx.raw.size(), difi::STANDARD_CONTEXT_SIZE);
    BOOST_CHECK_CLOSE(ctx.sample_rate, 2000000.0, 0.01);
    BOOST_CHECK_CLOSE(ctx.bandwidth, 2000000.0, 0.01); // Defaults to sample_rate
    BOOST_CHECK_CLOSE(ctx.rf_ref_freq, 0.0, 0.01);     // Defaults to 0
    BOOST_CHECK_CLOSE(ctx.if_ref_freq, 0.0, 0.01);     // Defaults to 0
    BOOST_CHECK_CLOSE(ctx.if_band_offset, 0.0, 0.01);  // Defaults to 0
    BOOST_CHECK_CLOSE(ctx.rf_gain, 0.0f, 0.01);        // Defaults to 0
    BOOST_CHECK_CLOSE(ctx.if_gain, 0.0f, 0.01);        // Defaults to 0
    BOOST_CHECK_CLOSE(ctx.ref_level, 0.0f, 0.01);      // Defaults to 0
    BOOST_CHECK_EQUAL(ctx.full, 0);
    BOOST_CHECK_EQUAL(ctx.frac, 0);
    BOOST_CHECK_EQUAL(ctx.timestamp_adjustment, 0);

    // Verify round-trip parsing
    standard_context parsed =
        difi::parse_context<context_version::standard>(ctx.raw.data(), ctx.raw.size());
    BOOST_CHECK_CLOSE(parsed.sample_rate, 2000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.bandwidth, 2000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_ref_freq, 0.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_ref_freq, 0.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_gain, 0.0f, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_gain, 0.0f, 0.01);
}

BOOST_AUTO_TEST_CASE(t_make_context_standard_custom)
{
    difi::context_config cfg{
        .sample_rate = 5000000.0,
        .depth = 16,
        .stream_id = 42,
        .rf_freq = 1500000000.0,
        .if_freq = 70000000.0,
        .bandwidth = 4000000.0,
        .if_band_offset = 10000.0,
        .rf_gain = 25.5f,
        .if_gain = -3.2f,
        .ref_level = 10.0f,
        .full = 1700000000,
        .frac = 500000000000ULL,
        .timestamp_adjustment = 15000000,
        .timestamp_calibration_time = 1699999000,
    };

    standard_context ctx = difi::make_context<context_version::standard>(cfg);

    BOOST_CHECK_EQUAL(ctx.packet_size, difi::STANDARD_CONTEXT_SIZE);

    standard_context parsed =
        difi::parse_context<context_version::standard>(ctx.raw.data(), ctx.raw.size());
    BOOST_CHECK_CLOSE(parsed.sample_rate, 5000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.bandwidth, 4000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_ref_freq, 1500000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_ref_freq, 70000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_band_offset, 10000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_gain, 25.5f, 0.1);
    BOOST_CHECK_CLOSE(parsed.if_gain, -3.2f, 0.1);
    BOOST_CHECK_CLOSE(parsed.ref_level, 10.0f, 0.1);
    BOOST_CHECK_EQUAL(parsed.full, 1700000000);
    BOOST_CHECK_EQUAL(parsed.frac, 500000000000ULL);
}

BOOST_AUTO_TEST_CASE(t_make_context_alternative)
{
    difi::context_config cfg{
        .sample_rate = 10000000.0,
        .depth = 8,
        .stream_id = 99,
        .rf_freq = 2400000000.0,
        .if_freq = 0.0,
        .bandwidth = 8000000.0,
    };

    alternative_context ctx = difi::make_context<context_version::alternative>(cfg);

    BOOST_CHECK_EQUAL(ctx.packet_size, difi::ALT_CONTEXT_SIZE);
    BOOST_CHECK_EQUAL(ctx.raw.size(), difi::ALT_CONTEXT_SIZE);

    alternative_context parsed =
        difi::parse_context<context_version::alternative>(ctx.raw.data(), ctx.raw.size());
    BOOST_CHECK_CLOSE(parsed.sample_rate, 10000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.bandwidth, 8000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.rf_ref_freq, 2400000000.0, 0.01);
    BOOST_CHECK_CLOSE(parsed.if_ref_freq, 0.0, 0.01);
}

BOOST_AUTO_TEST_CASE(t_make_context_runtime_variant)
{
    difi::context_config cfg{
        .sample_rate = 2000000.0,
        .depth = 16,
        .stream_id = 1,
    };

    auto var_std = difi::make_context(difi::context_version::standard, cfg);
    BOOST_CHECK(std::holds_alternative<standard_context>(var_std));
    BOOST_CHECK_EQUAL(std::get<standard_context>(var_std).packet_size, 108);

    auto var_alt = difi::make_context(difi::context_version::alternative, cfg);
    BOOST_CHECK(std::holds_alternative<alternative_context>(var_alt));
    BOOST_CHECK_EQUAL(std::get<alternative_context>(var_alt).packet_size, 72);
}

BOOST_AUTO_TEST_CASE(t_system_timestamp)
{
    auto [full1, frac1] = difi::system_timestamp(0);
    // Timestamp should be recent POSIX epoch seconds (greater than 2024-01-01: ~1704067200)
    BOOST_CHECK_GT(full1, 1704067200U);
    BOOST_CHECK_LT(frac1, difi::PICO_PER_SEC);

    // With 1 second (1,000,000 us) lead time
    auto [full2, frac2] = difi::system_timestamp(1000000);
    BOOST_CHECK_GE(full2, full1);
    if (full2 == full1) {
        BOOST_CHECK_GE(frac2, frac1);
    }
}

BOOST_AUTO_TEST_CASE(t_documented_enumerations_and_class_id)
{
    // Reference point identifiers per IEEE-ISTO 4900-2021 Section 5.2
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::reference_point::unspecified), 0U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::reference_point::antenna_feed), 25U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::reference_point::rf_converter), 75U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::reference_point::if_converter), 100U);

    // Packet classes per Section 4.3
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::standard_data), 0x0000U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::standard_context), 0x0001U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::sample_count_data), 0x0002U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::sample_count_context), 0x0003U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::version_context), 0x0004U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::sample_count_flow_control), 0x0005U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::packet_class::real_time_flow_control), 0x0006U);

    // Information classes per Section 4.2
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::information_class::basic_data_plane), 0x0000U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::information_class::version_flow), 0x0001U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::information_class::sample_count_data_plane), 0x0002U);
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(difi::information_class::bit_padding_data_plane), 0x0003U);

    // OUIs
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::oui::difi), 0x6a621eU);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::oui::vita), 0x7c386cU);

    // TSI and TSF types
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsi_type::none), 0);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsi_type::utc), 1);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsi_type::gps), 2);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsi_type::posix), 3);

    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsf_type::none), 0);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsf_type::sample_count), 1);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsf_type::picoseconds), 2);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(difi::tsf_type::free_running), 3);

    // CIF masks
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::standard_fields), 0x7bb98000U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::standard_with_change), 0xfbb98000U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::alternative_fields), 0x39a18000U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::alternative_fields), 966885376U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::alternative_with_change), 0xb9a18000U);
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::cif_mask::context_change), 0x80000000U);

    // State and event indicators
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(difi::state_and_event::default_indicators), 0xa00a0000U);
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(difi::state_and_event::default_indicators), 2685009920ULL);

    // Payload formats
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(difi::payload_format::sc8_link_efficient), 0xa00001c700000000ULL);
    BOOST_CHECK_EQUAL(static_cast<uint64_t>(payload_format::sc16_link_efficient), 0xa00003cf00000000ULL);

    // make_class_id tests
    uint64_t cid_data = difi::make_class_id(difi::oui::difi, difi::packet_class::standard_data);
    BOOST_CHECK_EQUAL(cid_data, 0x6a621e00000000ULL);

    uint64_t cid_ctx = difi::make_class_id(difi::oui::difi, difi::packet_class::standard_context);
    BOOST_CHECK_EQUAL(cid_ctx, (0x6a621eULL << 32) | 1ULL);

    uint64_t cid_pad = difi::make_class_id(difi::oui::difi, difi::packet_class::standard_data,
                                           difi::information_class::bit_padding_data_plane, 7);
    BOOST_CHECK_EQUAL((cid_pad >> 59) & 0x1fULL, 7ULL);
    BOOST_CHECK_EQUAL((cid_pad >> 16) & 0xffffULL, 3ULL);
    BOOST_CHECK_EQUAL(cid_pad & 0xffffULL, 0ULL);
}

BOOST_AUTO_TEST_CASE(t_reference_point_round_trip)
{
    // Test that reference point serializes and deserializes accurately
    difi::reference_point rps[] = {
        difi::reference_point::antenna_feed,
        difi::reference_point::rf_converter,
        difi::reference_point::if_converter,
        difi::reference_point::unspecified
    };

    for (auto rp : rps) {
        difi::context_config cfg{
            .sample_rate = 1000000.0,
            .depth = 16,
            .stream_id = 12,
            .ref_point = rp,
        };
        auto ctx = difi::make_context<difi::context_version::standard>(cfg);
        BOOST_CHECK(ctx.ref_point == (rp != difi::reference_point::unspecified ? rp : difi::reference_point::if_converter));

        auto parsed = difi::parse_context<difi::context_version::standard>(ctx.raw.data(), ctx.raw.size());
        BOOST_CHECK(parsed.ref_point == ctx.ref_point);

        // PMT round-trip
        difi_header hdr;
        hdr.stream_id = 12;
        auto pmt_dict = difi::context_to_pmt(hdr, ctx);
        auto from_pmt = difi::pmt_to_context<difi::context_version::standard>(pmt_dict);
        BOOST_CHECK(from_pmt.ref_point == ctx.ref_point);
        BOOST_CHECK(from_pmt.tsi == ctx.tsi);
    }
}

BOOST_AUTO_TEST_CASE(t_system_timestamp_time_sources)
{
    // POSIX timestamp
    auto [posix_full, posix_frac] = difi::system_timestamp(0, difi::tsi_type::posix);
    BOOST_CHECK(posix_full > 1700000000U);
    BOOST_CHECK(posix_frac < difi::PICO_CONVERSION);

    // UTC timestamp
    auto [utc_full, utc_frac] = difi::system_timestamp(0, difi::tsi_type::utc);
    BOOST_CHECK(utc_full > 1700000000U);
    BOOST_CHECK(utc_frac < difi::PICO_CONVERSION);

    // GPS timestamp (epoch Jan 6 1980; ~315964800s difference from 1970)
    auto [gps_full, gps_frac] = difi::system_timestamp(0, difi::tsi_type::gps);
    BOOST_CHECK(gps_full > 1300000000U && gps_full < posix_full);
    BOOST_CHECK(gps_frac < difi::PICO_CONVERSION);

    // Lead time addition
    auto [lead_full, lead_frac] = difi::system_timestamp(1000000, difi::tsi_type::posix);
    BOOST_CHECK(lead_full >= posix_full + 1 || (lead_full == posix_full && lead_frac > posix_frac));

    // make_data_static_bits and make_context_static_bits
    BOOST_CHECK_EQUAL(difi::make_data_static_bits(difi::tsi_type::posix), 0x18e00000U);
    BOOST_CHECK_EQUAL(difi::make_data_static_bits(difi::tsi_type::utc), 0x18600000U);
    BOOST_CHECK_EQUAL(difi::make_data_static_bits(difi::tsi_type::gps), 0x18a00000U);

    BOOST_CHECK_EQUAL(difi::make_context_static_bits(difi::tsi_type::posix), 0x49e00000U);
    BOOST_CHECK_EQUAL(difi::make_context_static_bits(difi::tsi_type::utc), 0x49600000U);
    BOOST_CHECK_EQUAL(difi::make_context_static_bits(difi::tsi_type::gps), 0x49a00000U);

    // Header parsing extracting TSI/TSF
    difi::header h_data;
    h_data.type = difi::packet_type::data;
    h_data.tsi = difi::tsi_type::utc;
    h_data.tsf = difi::tsf_type::picoseconds;
    std::vector<uint8_t> buf(difi::DIFI_HEADER_SIZE, 0);
    difi::pack_header(buf.data(), h_data);

    auto parsed_hdr = difi::parse_header(buf.data(), buf.size());
    BOOST_CHECK(parsed_hdr.tsi == difi::tsi_type::utc);
    BOOST_CHECK(parsed_hdr.tsf == difi::tsf_type::picoseconds);
}


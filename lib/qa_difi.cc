/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#define BOOST_TEST_MODULE qa_difi

#include <difi/difi.hpp>
#include <boost/test/unit_test.hpp>
#include <cmath>
#include <span>
#include <vector>

using namespace gr::difi;

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

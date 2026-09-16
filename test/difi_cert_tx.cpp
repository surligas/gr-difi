/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <difi/difi.hpp>
#include <udp_client.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace gr::difi;

int main(int argc, char* argv[])
{
    std::string ip = "127.0.0.1";
    uint16_t port = 50003;
    int bit_depth = 16;
    double sample_rate = 100000.0;
    int num_packets = 40;
    uint32_t stream_id = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc) {
            ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bit-depth" && i + 1 < argc) {
            bit_depth = std::stoi(argv[++i]);
        } else if (arg == "--sample-rate" && i + 1 < argc) {
            sample_rate = std::stod(argv[++i]);
        } else if (arg == "--num-packets" && i + 1 < argc) {
            num_packets = std::stoi(argv[++i]);
        } else if (arg == "--stream-id" && i + 1 < argc) {
            stream_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }

    std::cout << "[difi_cert_tx] Target: " << ip << ":" << port
              << " | bit_depth: " << bit_depth << " | sample_rate: " << sample_rate
              << " | num_packets: " << num_packets << std::endl;

    try {
        udp_client client(ip, port);

        uint32_t full_sec = 1740688471;
        uint64_t frac_pico = 200000000000ULL;
        uint8_t ctx_seq = 0;
        uint8_t data_seq = 0;

        // 1. Initial Signal Context packet
        standard_context ctx =
            difi::make_default_context<context_version::standard>(
                sample_rate, bit_depth, stream_id, full_sec, frac_pico);
        ctx.rf_ref_freq = 1950000000.0; // 1.95 GHz
        ctx.if_ref_freq = 0.0;
        ctx.bandwidth = sample_rate * 0.8;

        auto ctx_bytes = difi::pack_context(ctx, ctx_seq++, stream_id);
        client.send(ctx_bytes.data(), ctx_bytes.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // 2. Data packets setup
        // Small packet: 360 words total (28 bytes header + 1412 bytes payload)
        const size_t packet_size_words = 360;
        const size_t header_bytes = 28;
        const size_t payload_bytes = (packet_size_words - 7) * 4; // 1412 bytes
        const size_t bytes_per_sample = (bit_depth == 8) ? 2 : 4; // complex IQ
        const size_t samples_per_packet = payload_bytes / bytes_per_sample;

        std::vector<gr_complex> samples(samples_per_packet);
        for (size_t i = 0; i < samples_per_packet; ++i) {
            // Generate QPSK test pattern
            float re = ((i % 4 == 0 || i % 4 == 1) ? 1.0f : -1.0f) * 0.7f;
            float im = ((i % 4 == 0 || i % 4 == 2) ? 1.0f : -1.0f) * 0.7f;
            samples[i] = gr_complex(re, im);
        }

        std::vector<uint8_t> payload(payload_bytes, 0);
        difi::pack_samples(samples.data(), samples_per_packet, payload.data(), bit_depth);

        std::vector<uint8_t> packet_buf(packet_size_words * 4, 0);

        for (int p = 0; p < num_packets; ++p) {
            // Send periodic context packet every 10 data packets
            if (p > 0 && p % 10 == 0) {
                ctx.full = full_sec;
                ctx.frac = frac_pico;
                auto periodic_ctx = difi::pack_context(ctx, ctx_seq++ % 16, stream_id);
                client.send(periodic_ctx.data(), periodic_ctx.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            difi_header h;
            h.type = difi::packet_type::data;
            h.pkt_n = data_seq++ % 16;
            h.packet_size_words = static_cast<uint16_t>(packet_size_words);
            h.packet_size_bytes = packet_size_words * 4;
            h.stream_id = stream_id;
            h.class_id = (difi::STANDARD_OUI << 32); // InfoClass 0x0000, PacketClass 0x0000
            h.full = full_sec;
            h.frac = frac_pico;
            h.static_bits = difi::STANDARD_DATA_STATIC_BITS;

            difi::pack_header(packet_buf.data(), h);
            std::memcpy(packet_buf.data() + header_bytes, payload.data(), payload_bytes);

            client.send(packet_buf.data(), packet_buf.size());

            auto [next_full, next_frac] = difi::advance_timestamp(
                full_sec, frac_pico, samples_per_packet, sample_rate);
            full_sec = next_full;
            frac_pico = next_frac;

            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        std::cout << "[difi_cert_tx] Transmitted " << num_packets << " data packets and "
                  << static_cast<int>(ctx_seq) << " context packets successfully."
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[difi_cert_tx] Exception: " << e.what() << std::endl;
        return 1;
    }
}


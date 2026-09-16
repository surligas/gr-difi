/* -*- c++ -*- */
/*
 * Copyright (C) 2026 Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <difi/difi.hpp>
#include <udp_server.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace gr::difi;

int main(int argc, char* argv[])
{
    uint16_t port = 50003;
    int expected_bit_depth = 16;
    int min_packets = 20;
    int timeout_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--expected-bit-depth" && i + 1 < argc) {
            expected_bit_depth = std::stoi(argv[++i]);
        } else if (arg == "--min-packets" && i + 1 < argc) {
            min_packets = std::stoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::stoi(argv[++i]);
        }
    }

    std::cout << "[difi_cert_rx] Listening on port " << port
              << " | expected_bit_depth: " << expected_bit_depth
              << " | min_packets: " << min_packets
              << " | timeout_ms: " << timeout_ms << std::endl;

    try {
        udp_server server(port);

        int context_count = 0;
        int version_count = 0;
        int data_count = 0;
        int error_count = 0;
        int last_data_seq = -1;

        std::vector<uint8_t> buffer(65536);
        std::vector<gr_complex> samples(16384);

        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
            if (elapsed.count() >= timeout_ms) {
                std::cout << "[difi_cert_rx] Timeout reached (" << elapsed.count()
                          << " ms)." << std::endl;
                break;
            }

            ssize_t n = server.recv(buffer.data(), buffer.size());
            if (n < 0) {
                // Timeout on individual recv is normal (RECV_TIMEOUT_US = 100ms)
                continue;
            }

            if (static_cast<size_t>(n) < difi::HEADER_SIZE) {
                std::cerr << "[difi_cert_rx] Datagram smaller than DIFI header: " << n
                          << " bytes\n";
                error_count++;
                continue;
            }

            difi_header h;
            try {
                h = difi::parse_header(buffer.data(), static_cast<size_t>(n));
            } catch (const std::exception& e) {
                std::cerr << "[difi_cert_rx] Failed to parse header: " << e.what() << "\n";
                error_count++;
                continue;
            }

            if (h.type == difi::packet_type::context) {
                try {
                    auto ctx = difi::parse_context<context_version::standard>(
                        buffer.data(), static_cast<size_t>(n));
                    if (ctx.sample_rate <= 0.0) {
                        std::cerr << "[difi_cert_rx] Invalid sample rate in context: "
                                  << ctx.sample_rate << "\n";
                        error_count++;
                    }
                    context_count++;
                } catch (const std::exception& e) {
                    std::cerr << "[difi_cert_rx] Failed to parse context: " << e.what()
                              << "\n";
                    error_count++;
                }
            } else if (h.type == difi::packet_type::version) {
                version_count++;
            } else if (h.type == difi::packet_type::data) {
                if (last_data_seq != -1) {
                    int expected_seq = (last_data_seq + 1) % 16;
                    if (h.pkt_n != expected_seq) {
                        std::cerr << "[difi_cert_rx] Sequence jump: expected "
                                  << expected_seq << ", got "
                                  << static_cast<int>(h.pkt_n) << "\n";
                        error_count++;
                    }
                }
                last_data_seq = h.pkt_n;

                size_t payload_bytes = static_cast<size_t>(n) - difi::HEADER_SIZE;
                size_t unpacked = difi::unpack_samples(buffer.data() + difi::HEADER_SIZE,
                                                       payload_bytes,
                                                       samples.data(),
                                                       samples.size(),
                                                       expected_bit_depth);
                if (unpacked == 0) {
                    std::cerr << "[difi_cert_rx] Unpacked 0 samples from data packet\n";
                    error_count++;
                }
                data_count++;
            }

            if (context_count >= 1 && data_count >= min_packets) {
                std::cout
                    << "[difi_cert_rx] Sufficient packets received. Exiting successfully.\n";
                break;
            }
        }

        std::cout << "[difi_cert_rx] Summary: Context=" << context_count
                  << ", Version=" << version_count << ", Data=" << data_count
                  << ", Errors=" << error_count << std::endl;

        if (context_count > 0 && data_count >= min_packets && error_count == 0) {
            std::cout << "[difi_cert_rx] Validation PASSED.\n";
            return 0;
        } else {
            std::cerr << "[difi_cert_rx] Validation FAILED.\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "[difi_cert_rx] Fatal error: " << e.what() << std::endl;
        return 1;
    }
}


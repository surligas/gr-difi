// -*- c++ -*- //
// Copyright (c) Microsoft Corporation and Welkin Sciences, LLC.
// Licensed under the GNU General Public License v3.0 or later.
// See License.txt in the project root for license information.
//

#include <pybind11/complex.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

#include <difi/difi_sink_cpp.h>
#include <difi_sink_cpp_pydoc.h>

void bind_difi_sink_cpp(py::module& m)
{
    using difi_sink_base = ::gr::difi::difi_sink_base;
    using standard_context = ::gr::difi::difi::standard_context;
    using alternative_context = ::gr::difi::difi::alternative_context;
    using namespace ::gr::difi;

    py::class_<standard_context>(m, "standard_context")
        .def(py::init<>())
        .def_readwrite("class_id", &standard_context::class_id)
        .def_readwrite("full", &standard_context::full)
        .def_readwrite("frac", &standard_context::frac)
        .def_readwrite("cif", &standard_context::cif)
        .def_readwrite("ref_point", &standard_context::ref_point)
        .def_readwrite("bandwidth", &standard_context::bandwidth)
        .def_readwrite("if_ref_freq", &standard_context::if_ref_freq)
        .def_readwrite("rf_ref_freq", &standard_context::rf_ref_freq)
        .def_readwrite("if_band_offset", &standard_context::if_band_offset)
        .def_readwrite("ref_level", &standard_context::ref_level)
        .def_readwrite("rf_gain", &standard_context::rf_gain)
        .def_readwrite("if_gain", &standard_context::if_gain)
        .def_readwrite("sample_rate", &standard_context::sample_rate)
        .def_readwrite("timestamp_adjustment", &standard_context::timestamp_adjustment)
        .def_readwrite("timestamp_calibration_time", &standard_context::timestamp_calibration_time)
        .def_readwrite("state_and_event_indicators", &standard_context::state_and_event_indicators)
        .def_readwrite("payload_format", &standard_context::payload_format)
        .def_readwrite("packet_size", &standard_context::packet_size);

    py::class_<alternative_context>(m, "alternative_context")
        .def(py::init<>())
        .def_readwrite("class_id", &alternative_context::class_id)
        .def_readwrite("cif", &alternative_context::cif)
        .def_readwrite("bandwidth", &alternative_context::bandwidth)
        .def_readwrite("if_ref_freq", &alternative_context::if_ref_freq)
        .def_readwrite("rf_ref_freq", &alternative_context::rf_ref_freq)
        .def_readwrite("if_band_offset", &alternative_context::if_band_offset)
        .def_readwrite("sample_rate", &alternative_context::sample_rate)
        .def_readwrite("state_and_event_indicators", &alternative_context::state_and_event_indicators)
        .def_readwrite("payload_format", &alternative_context::payload_format)
        .def_readwrite("packet_size", &alternative_context::packet_size);

    auto make_dispatch = [](
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        uint32_t reference_time_full,
        uint64_t reference_time_frac,
        bool burst_mode,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol,
        int bit_depth_int
    ) -> difi_sink_base::sptr {
        bool is_alt = (context_version_int == 72 || context_version_int == static_cast<int>(difi::context_version::alternative));
        bool is_tcp = (protocol == 1);
        bool is_8bit = (bit_depth_int == 8);

        if (is_alt) {
            if (is_tcp) {
                if (is_8bit) return difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
                else return difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
            } else {
                if (is_8bit) return difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
                else return difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
            }
        } else {
            if (is_tcp) {
                if (is_8bit) return difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
                else return difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
            } else {
                if (is_8bit) return difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
                else return difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    reference_time_full, reference_time_frac, burst_mode, lead_time_us);
            }
        }
    };

    auto make_sc8 = [make_dispatch](
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        uint32_t reference_time_full,
        uint64_t reference_time_frac,
        bool burst_mode,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol
    ) {
        return make_dispatch(ip_addr, port, samp_rate, samples_per_packet,
                             stream_number, context_interval, reference_time_full,
                             reference_time_frac, burst_mode, lead_time_us,
                             context_version_int, protocol, 8);
    };

    auto make_sc16 = [make_dispatch](
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        uint32_t reference_time_full,
        uint64_t reference_time_frac,
        bool burst_mode,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol
    ) {
        return make_dispatch(ip_addr, port, samp_rate, samples_per_packet,
                             stream_number, context_interval, reference_time_full,
                             reference_time_frac, burst_mode, lead_time_us,
                             context_version_int, protocol, 16);
    };

    py::class_<difi_sink_base, gr::sync_block, gr::block, gr::basic_block,
        std::shared_ptr<difi_sink_base>>(m, "difi_sink_base", D(difi_sink_cpp))
        .def(py::init(make_dispatch),
           py::arg("ip_addr"),
           py::arg("port"),
           py::arg("samp_rate"),
           py::arg("samples_per_packet"),
           py::arg("stream_number") = 0,
           py::arg("context_interval") = 100,
           py::arg("reference_time_full") = 0,
           py::arg("reference_time_frac") = 0,
           py::arg("burst_mode") = false,
           py::arg("lead_time_us") = 10000,
           py::arg("context_version") = 108,
           py::arg("protocol") = 2,
           py::arg("bit_depth") = 16
        )
        .def_static("make", make_dispatch,
           py::arg("ip_addr"),
           py::arg("port"),
           py::arg("samp_rate"),
           py::arg("samples_per_packet"),
           py::arg("stream_number") = 0,
           py::arg("context_interval") = 100,
           py::arg("reference_time_full") = 0,
           py::arg("reference_time_frac") = 0,
           py::arg("burst_mode") = false,
           py::arg("lead_time_us") = 10000,
           py::arg("context_version") = 108,
           py::arg("protocol") = 2,
           py::arg("bit_depth") = 16
        )
        .def("set_burst_mode", &difi_sink_base::set_burst_mode, py::arg("enable"))
        .def("burst_mode", &difi_sink_base::burst_mode)
        .def("set_lead_time_us", &difi_sink_base::set_lead_time_us, py::arg("lead_time_us"))
        .def("lead_time_us", &difi_sink_base::lead_time_us)
        .def("context", &difi_sink_base::context)
        .def("set_context", &difi_sink_base::set_context, py::arg("ctx"));

    m.def("difi_sink_cpp", make_dispatch,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("reference_time_full") = 0,
       py::arg("reference_time_frac") = 0,
       py::arg("burst_mode") = false,
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2,
       py::arg("bit_depth") = 16
    );

    m.def("difi_sink_cpp_sc8", make_sc8,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("reference_time_full") = 0,
       py::arg("reference_time_frac") = 0,
       py::arg("burst_mode") = false,
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2
    );

    m.def("difi_sink_cpp_sc16", make_sc16,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("reference_time_full") = 0,
       py::arg("reference_time_frac") = 0,
       py::arg("burst_mode") = false,
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2
    );
}

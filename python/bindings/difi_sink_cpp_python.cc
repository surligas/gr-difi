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

    py::enum_<difi::reference_point>(m, "reference_point", py::arithmetic())
        .value("unspecified", difi::reference_point::unspecified)
        .value("antenna_feed", difi::reference_point::antenna_feed)
        .value("rf_converter", difi::reference_point::rf_converter)
        .value("if_converter", difi::reference_point::if_converter);

    py::enum_<difi::packet_class>(m, "packet_class", py::arithmetic())
        .value("standard_data", difi::packet_class::standard_data)
        .value("standard_context", difi::packet_class::standard_context)
        .value("sample_count_data", difi::packet_class::sample_count_data)
        .value("sample_count_context", difi::packet_class::sample_count_context)
        .value("version_context", difi::packet_class::version_context)
        .value("sample_count_flow_control", difi::packet_class::sample_count_flow_control)
        .value("real_time_flow_control", difi::packet_class::real_time_flow_control);

    py::enum_<difi::information_class>(m, "information_class", py::arithmetic())
        .value("basic_data_plane", difi::information_class::basic_data_plane)
        .value("version_flow", difi::information_class::version_flow)
        .value("sample_count_data_plane", difi::information_class::sample_count_data_plane)
        .value("bit_padding_data_plane", difi::information_class::bit_padding_data_plane);

    py::enum_<difi::oui>(m, "oui", py::arithmetic())
        .value("difi", difi::oui::difi)
        .value("vita", difi::oui::vita);

    py::enum_<difi::packet_type>(m, "packet_type", py::arithmetic())
        .value("unknown", difi::packet_type::unknown)
        .value("data", difi::packet_type::data)
        .value("context", difi::packet_type::context)
        .value("version", difi::packet_type::version)
        .value("command", difi::packet_type::command);

    py::enum_<difi::bit_depth>(m, "bit_depth", py::arithmetic())
        .value("bits_8", difi::bit_depth::bits_8)
        .value("bits_16", difi::bit_depth::bits_16);

    py::enum_<difi::context_version>(m, "context_version", py::arithmetic())
        .value("standard", difi::context_version::standard)
        .value("alternative", difi::context_version::alternative);

    py::enum_<difi::context_behavior>(m, "context_behavior", py::arithmetic())
        .value("throw_exe", difi::context_behavior::throw_exe)
        .value("ignore", difi::context_behavior::ignore)
        .value("warnings_forward", difi::context_behavior::warnings_forward)
        .value("warnings_no_forward", difi::context_behavior::warnings_no_forward);

    py::enum_<difi::tsi_type>(m, "tsi_type", py::arithmetic())
        .value("none", difi::tsi_type::none)
        .value("utc", difi::tsi_type::utc)
        .value("gps", difi::tsi_type::gps)
        .value("posix", difi::tsi_type::posix);

    py::enum_<difi::tsf_type>(m, "tsf_type", py::arithmetic())
        .value("none", difi::tsf_type::none)
        .value("sample_count", difi::tsf_type::sample_count)
        .value("picoseconds", difi::tsf_type::picoseconds)
        .value("free_running", difi::tsf_type::free_running);

    py::enum_<difi::cif_mask>(m, "cif_mask", py::arithmetic())
        .value("context_change", difi::cif_mask::context_change)
        .value("standard_fields", difi::cif_mask::standard_fields)
        .value("standard_with_change", difi::cif_mask::standard_with_change)
        .value("alternative_fields", difi::cif_mask::alternative_fields)
        .value("alternative_with_change", difi::cif_mask::alternative_with_change);

    py::enum_<difi::state_and_event>(m, "state_and_event", py::arithmetic())
        .value("calibrated_time_enable", difi::state_and_event::calibrated_time_enable)
        .value("reference_lock_enable", difi::state_and_event::reference_lock_enable)
        .value("calibrated_time_indicator", difi::state_and_event::calibrated_time_indicator)
        .value("reference_lock_indicator", difi::state_and_event::reference_lock_indicator)
        .value("default_indicators", difi::state_and_event::default_indicators);

    py::enum_<difi::payload_format>(m, "payload_format", py::arithmetic())
        .value("sc8_link_efficient", difi::payload_format::sc8_link_efficient)
        .value("sc16_link_efficient", difi::payload_format::sc16_link_efficient);

    py::class_<difi::context_config>(m, "context_config")
        .def(py::init<>())
        .def_readwrite("sample_rate", &difi::context_config::sample_rate)
        .def_readwrite("depth", &difi::context_config::depth)
        .def_readwrite("stream_id", &difi::context_config::stream_id)
        .def_readwrite("rf_freq", &difi::context_config::rf_freq)
        .def_readwrite("if_freq", &difi::context_config::if_freq)
        .def_readwrite("bandwidth", &difi::context_config::bandwidth)
        .def_readwrite("if_band_offset", &difi::context_config::if_band_offset)
        .def_readwrite("rf_gain", &difi::context_config::rf_gain)
        .def_readwrite("if_gain", &difi::context_config::if_gain)
        .def_readwrite("ref_level", &difi::context_config::ref_level)
        .def_readwrite("full", &difi::context_config::full)
        .def_readwrite("frac", &difi::context_config::frac)
        .def_readwrite("timestamp_adjustment", &difi::context_config::timestamp_adjustment)
        .def_readwrite("timestamp_calibration_time", &difi::context_config::timestamp_calibration_time)
        .def_readwrite("class_id", &difi::context_config::class_id)
        .def_readwrite("cif", &difi::context_config::cif)
        .def_readwrite("ref_point", &difi::context_config::ref_point)
        .def_readwrite("state_and_event_indicators", &difi::context_config::state_and_event_indicators)
        .def_readwrite("tsi", &difi::context_config::tsi);

    m.def("make_class_id", &difi::make_class_id,
          py::arg("id_oui") = difi::oui::difi,
          py::arg("pkt_cls") = difi::packet_class::standard_data,
          py::arg("info_cls") = difi::information_class::basic_data_plane,
          py::arg("pad_bits") = 0);

    m.def("make_data_static_bits", &difi::make_data_static_bits,
          py::arg("tsi") = difi::tsi_type::posix,
          py::arg("tsf") = difi::tsf_type::picoseconds);

    m.def("make_context_static_bits", &difi::make_context_static_bits,
          py::arg("tsi") = difi::tsi_type::posix,
          py::arg("tsf") = difi::tsf_type::picoseconds);

    m.def("system_timestamp", &difi::system_timestamp,
          py::arg("lead_time_us") = 0,
          py::arg("tsi") = difi::tsi_type::posix);

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
        .def_readwrite("tsi", &standard_context::tsi)
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

    py::class_<difi::flow_control>(m, "flow_control")
        .def(py::init<>())
        .def_readwrite("type", &difi::flow_control::type)
        .def_readwrite("pkt_n", &difi::flow_control::pkt_n)
        .def_readwrite("stream_id", &difi::flow_control::stream_id)
        .def_readwrite("class_id", &difi::flow_control::class_id)
        .def_readwrite("message_id", &difi::flow_control::message_id)
        .def_readwrite("controllee_id", &difi::flow_control::controllee_id)
        .def_readwrite("controller_id", &difi::flow_control::controller_id)
        .def_readwrite("full", &difi::flow_control::full)
        .def_readwrite("frac", &difi::flow_control::frac)
        .def_readwrite("cif0", &difi::flow_control::cif0)
        .def_readwrite("cif1", &difi::flow_control::cif1)
        .def_readwrite("ref_point", &difi::flow_control::ref_point)
        .def_readwrite("sample_rate", &difi::flow_control::sample_rate)
        .def_readwrite("timestamp_adjustment", &difi::flow_control::timestamp_adjustment)
        .def_readwrite("buffer_size", &difi::flow_control::buffer_size)
        .def_readwrite("buffer_level", &difi::flow_control::buffer_level)
        .def_readwrite("overflow", &difi::flow_control::overflow)
        .def_readwrite("nearly_full", &difi::flow_control::nearly_full)
        .def_readwrite("nearly_empty", &difi::flow_control::nearly_empty)
        .def_readwrite("underflow", &difi::flow_control::underflow)
        .def_readwrite("tsi", &difi::flow_control::tsi)
        .def_readwrite("tsf", &difi::flow_control::tsf)
        .def("buffer_fill_pct", &difi::flow_control::buffer_fill_pct);

    m.def("pack_flow_control", [](const difi::flow_control& fc) {
        auto vec = difi::pack_flow_control(fc);
        return py::bytes(reinterpret_cast<const char*>(vec.data()), vec.size());
    }, py::arg("fc"));

    m.def("parse_flow_control", [](py::bytes data) -> std::optional<difi::flow_control> {
        std::string s = data;
        return difi::parse_flow_control(s.data(), s.size());
    }, py::arg("data"));

    auto make_dispatch = [](
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        std::string length_tag_name,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol,
        double rf_freq,
        double if_freq,
        double bandwidth,
        float rf_gain,
        float if_gain,
        float ref_level,
        double if_band_offset,
        int bit_depth_int,
        int ref_point_int,
        int time_source_int,
        bool rate_control,
        float max_buffer_fill_pct
    ) -> difi_sink_base::sptr {
        bool is_alt = (context_version_int == 72 || context_version_int == static_cast<int>(difi::context_version::alternative));
        bool is_tcp = (protocol == 1);
        bool is_8bit = (bit_depth_int == 8);
        difi::reference_point rp = static_cast<difi::reference_point>(ref_point_int);
        difi::tsi_type tsi = static_cast<difi::tsi_type>(time_source_int);

        if (is_alt) {
            if (is_tcp) {
                if (is_8bit) return difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
                else return difi_sink_cpp<difi::context_version::alternative, tcp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
            } else {
                if (is_8bit) return difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
                else return difi_sink_cpp<difi::context_version::alternative, udp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
            }
        } else {
            if (is_tcp) {
                if (is_8bit) return difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
                else return difi_sink_cpp<difi::context_version::standard, tcp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
            } else {
                if (is_8bit) return difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_8>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
                else return difi_sink_cpp<difi::context_version::standard, udp_client, difi::bit_depth::bits_16>::make(
                    ip_addr, port, samp_rate, samples_per_packet, stream_number, context_interval,
                    length_tag_name, lead_time_us,
                    rf_freq, if_freq, bandwidth, rf_gain, if_gain, ref_level, if_band_offset, rp, tsi, rate_control, max_buffer_fill_pct);
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
        std::string length_tag_name,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol,
        double rf_freq,
        double if_freq,
        double bandwidth,
        float rf_gain,
        float if_gain,
        float ref_level,
        double if_band_offset,
        int ref_point_int,
        int time_source_int,
        bool rate_control,
        float max_buffer_fill_pct
    ) {
        return make_dispatch(ip_addr, port, samp_rate, samples_per_packet,
                             stream_number, context_interval,
                             length_tag_name, lead_time_us,
                             context_version_int, protocol,
                             rf_freq, if_freq, bandwidth,
                             rf_gain, if_gain, ref_level, if_band_offset,
                             8, ref_point_int, time_source_int,
                             rate_control, max_buffer_fill_pct);
    };

    auto make_sc16 = [make_dispatch](
        std::string ip_addr,
        uint16_t port,
        uint64_t samp_rate,
        uint32_t samples_per_packet,
        int stream_number,
        int context_interval,
        std::string length_tag_name,
        uint32_t lead_time_us,
        int context_version_int,
        uint8_t protocol,
        double rf_freq,
        double if_freq,
        double bandwidth,
        float rf_gain,
        float if_gain,
        float ref_level,
        double if_band_offset,
        int ref_point_int,
        int time_source_int,
        bool rate_control,
        float max_buffer_fill_pct
    ) {
        return make_dispatch(ip_addr, port, samp_rate, samples_per_packet,
                             stream_number, context_interval,
                             length_tag_name, lead_time_us,
                             context_version_int, protocol,
                             rf_freq, if_freq, bandwidth,
                             rf_gain, if_gain, ref_level, if_band_offset,
                             16, ref_point_int, time_source_int,
                             rate_control, max_buffer_fill_pct);
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
           py::arg("length_tag_name") = "",
           py::arg("lead_time_us") = 10000,
           py::arg("context_version") = 108,
           py::arg("protocol") = 2,
           py::arg("rf_freq") = 0.0,
           py::arg("if_freq") = 0.0,
           py::arg("bandwidth") = 0.0,
           py::arg("rf_gain") = 0.0f,
           py::arg("if_gain") = 0.0f,
           py::arg("ref_level") = 0.0f,
           py::arg("if_band_offset") = 0.0,
           py::arg("bit_depth") = 16,
           py::arg("ref_point") = static_cast<int>(difi::reference_point::if_converter),
           py::arg("time_source") = static_cast<int>(difi::tsi_type::posix),
           py::arg("rate_control") = false,
           py::arg("max_buffer_fill_pct") = 50.0f
        )
        .def_static("make", make_dispatch,
           py::arg("ip_addr"),
           py::arg("port"),
           py::arg("samp_rate"),
           py::arg("samples_per_packet"),
           py::arg("stream_number") = 0,
           py::arg("context_interval") = 100,
           py::arg("length_tag_name") = "",
           py::arg("lead_time_us") = 10000,
           py::arg("context_version") = 108,
           py::arg("protocol") = 2,
           py::arg("rf_freq") = 0.0,
           py::arg("if_freq") = 0.0,
           py::arg("bandwidth") = 0.0,
           py::arg("rf_gain") = 0.0f,
           py::arg("if_gain") = 0.0f,
           py::arg("ref_level") = 0.0f,
           py::arg("if_band_offset") = 0.0,
           py::arg("bit_depth") = 16,
           py::arg("ref_point") = static_cast<int>(difi::reference_point::if_converter),
           py::arg("time_source") = static_cast<int>(difi::tsi_type::posix),
           py::arg("rate_control") = false,
           py::arg("max_buffer_fill_pct") = 50.0f
        )
        .def("set_length_tag_name", &difi_sink_base::set_length_tag_name, py::arg("tag_name"))
        .def("length_tag_name", &difi_sink_base::length_tag_name)
        .def("burst_mode", &difi_sink_base::burst_mode)
        .def("set_lead_time_us", &difi_sink_base::set_lead_time_us, py::arg("lead_time_us"))
        .def("lead_time_us", &difi_sink_base::lead_time_us)
        .def("context", &difi_sink_base::context)
        .def("set_context", &difi_sink_base::set_context, py::arg("ctx"))
        .def("set_rf_freq", &difi_sink_base::set_rf_freq, py::arg("freq"))
        .def("rf_freq", &difi_sink_base::rf_freq)
        .def("set_center_freq", &difi_sink_base::set_center_freq, py::arg("freq"))
        .def("center_freq", &difi_sink_base::center_freq)
        .def("set_if_freq", &difi_sink_base::set_if_freq, py::arg("freq"))
        .def("if_freq", &difi_sink_base::if_freq)
        .def("set_bandwidth", &difi_sink_base::set_bandwidth, py::arg("bw"))
        .def("bandwidth", &difi_sink_base::bandwidth)
        .def("set_rf_gain", &difi_sink_base::set_rf_gain, py::arg("gain"))
        .def("rf_gain", &difi_sink_base::rf_gain)
        .def("set_gain", &difi_sink_base::set_gain, py::arg("gain"))
        .def("gain", &difi_sink_base::gain)
        .def("set_if_gain", &difi_sink_base::set_if_gain, py::arg("gain"))
        .def("if_gain", &difi_sink_base::if_gain)
        .def("set_ref_level", &difi_sink_base::set_ref_level, py::arg("level"))
        .def("ref_level", &difi_sink_base::ref_level)
        .def("set_if_band_offset", &difi_sink_base::set_if_band_offset, py::arg("offset"))
        .def("if_band_offset", &difi_sink_base::if_band_offset)
        .def("set_ref_point", &difi_sink_base::set_ref_point, py::arg("ref_point"))
        .def("ref_point", &difi_sink_base::ref_point)
        .def("set_time_source", &difi_sink_base::set_time_source, py::arg("time_source"))
        .def("time_source", &difi_sink_base::time_source)
        .def("set_tsi", &difi_sink_base::set_tsi, py::arg("tsi"))
        .def("tsi", &difi_sink_base::tsi)
        .def("set_rate_control", &difi_sink_base::set_rate_control, py::arg("enabled"))
        .def("rate_control", &difi_sink_base::rate_control)
        .def("set_max_buffer_fill_pct", &difi_sink_base::set_max_buffer_fill_pct, py::arg("pct"))
        .def("max_buffer_fill_pct", &difi_sink_base::max_buffer_fill_pct)
        .def("last_buffer_fill_pct", &difi_sink_base::last_buffer_fill_pct)
        .def("last_buffer_level", &difi_sink_base::last_buffer_level)
        .def("device_buffer_size", &difi_sink_base::device_buffer_size)
        .def("device_nearly_full", &difi_sink_base::device_nearly_full)
        .def("device_nearly_empty", &difi_sink_base::device_nearly_empty)
        .def("device_overflow", &difi_sink_base::device_overflow)
        .def("device_underflow", &difi_sink_base::device_underflow);

    m.def("difi_sink_cpp", make_dispatch,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("length_tag_name") = "",
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2,
       py::arg("rf_freq") = 0.0,
       py::arg("if_freq") = 0.0,
       py::arg("bandwidth") = 0.0,
       py::arg("rf_gain") = 0.0f,
       py::arg("if_gain") = 0.0f,
       py::arg("ref_level") = 0.0f,
       py::arg("if_band_offset") = 0.0,
       py::arg("bit_depth") = 16,
       py::arg("ref_point") = static_cast<int>(difi::reference_point::if_converter),
       py::arg("time_source") = static_cast<int>(difi::tsi_type::posix),
       py::arg("rate_control") = false,
       py::arg("max_buffer_fill_pct") = 50.0f
    );

    m.def("difi_sink_cpp_sc8", make_sc8,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("length_tag_name") = "",
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2,
       py::arg("rf_freq") = 0.0,
       py::arg("if_freq") = 0.0,
       py::arg("bandwidth") = 0.0,
       py::arg("rf_gain") = 0.0f,
       py::arg("if_gain") = 0.0f,
       py::arg("ref_level") = 0.0f,
       py::arg("if_band_offset") = 0.0,
       py::arg("ref_point") = static_cast<int>(difi::reference_point::if_converter),
       py::arg("time_source") = static_cast<int>(difi::tsi_type::posix),
       py::arg("rate_control") = false,
       py::arg("max_buffer_fill_pct") = 50.0f
    );

    m.def("difi_sink_cpp_sc16", make_sc16,
       py::arg("ip_addr"),
       py::arg("port"),
       py::arg("samp_rate"),
       py::arg("samples_per_packet"),
       py::arg("stream_number") = 0,
       py::arg("context_interval") = 100,
       py::arg("length_tag_name") = "",
       py::arg("lead_time_us") = 10000,
       py::arg("context_version") = 108,
       py::arg("protocol") = 2,
       py::arg("rf_freq") = 0.0,
       py::arg("if_freq") = 0.0,
       py::arg("bandwidth") = 0.0,
       py::arg("rf_gain") = 0.0f,
       py::arg("if_gain") = 0.0f,
       py::arg("ref_level") = 0.0f,
       py::arg("if_band_offset") = 0.0,
       py::arg("ref_point") = static_cast<int>(difi::reference_point::if_converter),
       py::arg("time_source") = static_cast<int>(difi::tsi_type::posix),
       py::arg("rate_control") = false,
       py::arg("max_buffer_fill_pct") = 50.0f
    );
}

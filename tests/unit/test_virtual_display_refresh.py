"""Compile Windows refresh resolution with production parsing and helper bodies.

Only platform APIs and display types are stubbed. Run separate processes for OS
builds because the production helper caches the Windows version.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "src/platform/windows/display_helper_request_helpers.cpp").read_text()


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


refresh = function((root / "src/rtsp.h").read_text(), "inline std::uint32_t effective_display_refresh_millihz(")
program = r'''
#include <cassert>
#include <iostream>
#include <type_traits>
#include "src/display_device_policy.h"
#include "src/platform/windows/virtual_display_refresh_policy.h"
#include "src/platform/windows/display_helper_request_policy.h"
#define BOOST_LOG(level) std::clog
namespace config { using video_t = display_device::policy::video_config_t; }
namespace rtsp_stream {
 struct launch_session_t : display_device::policy::session_t {
   int framegen_refresh_multiplier = 1;
   bool framegen_fixed_refresh = false;
 };
 bool rtx_hdr_enabled(const config::video_t &v) { return v.rtx_hdr_enabled; }
 bool effective_hdr_requested(const launch_session_t &s) { return display_device::policy::effective_hdr_requested(s); }
''' + refresh + r'''
}
namespace platf {
 struct version_t { std::optional<std::uint32_t> build_number; };
 version_t query_windows_version() { return {BUILD}; }
}
namespace display_device {
 using Resolution = policy::resolution_t;
 using Rational = policy::rational_t;
 using FloatingPoint = std::variant<Rational, double>;
 using HdrState = policy::hdr_state_e;
 struct SingleDisplayConfiguration {
   std::optional<Resolution> m_resolution;
   std::optional<FloatingPoint> m_refresh_rate;
   std::optional<HdrState> m_hdr_state;
 };
 auto parse_configuration(const config::video_t &v, const rtsp_stream::launch_session_t &s) {
   std::variant<policy::failed_to_parse_tag_t, policy::configuration_disabled_tag_t, SingleDisplayConfiguration> out;
   const auto result = policy::parse_configuration(v, s);
   if (const auto *cfg = std::get_if<policy::configuration_t>(&result)) {
     SingleDisplayConfiguration c {cfg->m_resolution, std::nullopt, cfg->m_hdr_state};
     if (cfg->m_refresh_rate) c.m_refresh_rate = *cfg->m_refresh_rate;
     out = c;
   }
   return out;
 }
 bool refresh_rate_override_active(const config::video_t &v, const rtsp_stream::launch_session_t &s) {
   return policy::refresh_rate_override_active(v, s);
 }
}
namespace display_helper_integration::helpers {
''' + function(source, "double get_refresh_rate_value(") + function(source, "void limit_virtual_display_refresh(") + r'''
 class SessionDisplayConfigurationHelper {
 public:
   SessionDisplayConfigurationHelper(const config::video_t &v, const rtsp_stream::launch_session_t &s, bool)
     : effective_video_config_(v), session_(s) {}
   std::optional<display_device::SingleDisplayConfiguration> initial_virtual_display_configuration() const;
 private:
   config::video_t effective_video_config_;
   const rtsp_stream::launch_session_t &session_;
 };
''' + function(source, "std::optional<display_device::SingleDisplayConfiguration> SessionDisplayConfigurationHelper::initial_virtual_display_configuration()") + function(source, "void resolve_virtual_display_refresh(") + r'''
}
int main() {
 namespace policy = display_device::policy;
 namespace helpers = display_helper_integration::helpers;
 config::video_t video;
 video.dd.configuration_option = policy::video_config_t::dd_t::config_option_e::ensure_active;
 video.dd.resolution_option = policy::video_config_t::dd_t::resolution_option_e::automatic;
 video.dd.refresh_rate_option = policy::video_config_t::dd_t::refresh_rate_option_e::automatic;
 auto check = [&](int height, unsigned requested, unsigned expected, bool fixed) {
   rtsp_stream::launch_session_t session;
   session.width = 3840; session.height = height; session.fps = 60;
   session.framegen_refresh_millihz = requested;
   session.framegen_refresh_rate = framegen::rounded_fps_from_millihz(requested);
   session.framegen_refresh_multiplier = requested == 240000 ? 4 : 1;
   helpers::resolve_virtual_display_refresh(video, session);
   assert(rtsp_stream::effective_display_refresh_millihz(session) == expected);
   assert(session.fps == 60);
   assert(session.framegen_fixed_refresh == fixed);
   const unsigned base = session.framegen_fixed_refresh ? expected : 60000;
   const auto creation = VDISPLAY::policy::resolve_creation_refresh(expected, base,
     session.framegen_refresh_multiplier, height, BUILD);
   assert(creation.requested_millihz == expected);
   const auto cfg = helpers::SessionDisplayConfigurationHelper(video, session, true).initial_virtual_display_configuration();
   assert(cfg && cfg->m_refresh_rate);
   assert(std::abs(helpers::get_refresh_rate_value(*cfg->m_refresh_rate) * 1000 - expected) < 0.01);
 };
 check(2160, 240000, 240000, false);
 check(2160, 1000000, BUILD < 26100 ? 462962 : 1000000, BUILD < 26100);
 video.dd.mode_remapping.mixed = {{"3840x1440", "60", "", "480"}, {"", "120", "", "1000"}};
 check(1440, 1000000, 480000, true);
 video.dd.mode_remapping.mixed = {{"", "60", "", "120"}};
 check(2160, 240000, 120000, true); // Explicit rate can be below automatic minimum.
 video.dd.mode_remapping.mixed = {{"", "60", "", "479.520"}};
 check(1440, 1000000, 479520, true);
 video.dd.mode_remapping.mixed = {{"", "60", "3840x2160", "1000"}};
 // Limit uses the final resolution, not the client height.
 check(1440, 1000000, BUILD < 26100 ? 462962 : 1000000, true);
 video.dd.refresh_rate_option = policy::video_config_t::dd_t::refresh_rate_option_e::manual;
 video.dd.manual_refresh_rate = "480";
 check(2160, 1000000, BUILD < 26100 ? 462962 : 480000, true);
 std::cout << "Windows refresh helpers passed for build " << BUILD << "\n";
}
'''
with tempfile.TemporaryDirectory(prefix="virtual-display-refresh-") as temporary:
    directory = pathlib.Path(temporary)
    cpp = directory / "test.cpp"
    cpp.write_text(program)
    for build in (19045, 22631, 26100):
        binary = directory / f"test-{build}"
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", f"-DBUILD={build}", "-I", str(root), str(cpp), str(root / "src/display_device_policy.cpp"), str(root / "src/platform/windows/display_helper_request_policy.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

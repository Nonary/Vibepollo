"""Execute the complete production backend selector with inert capture/session edges."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else 'g++'
source = (root / 'src/platform/linux/display_backend.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.lstrip().startswith('#include'))
# Keep the production virtual interface and capability shape; only external
# data structures/capture discovery and KScreen manager are deterministic.
interface = (root / 'src/platform/linux/display_backend.h').read_text()
interface = '\n'.join(line for line in interface.splitlines() if not line.lstrip().startswith(('#include', '#pragma')))
program = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <iostream>
#include "src/hdr_stream_policy.h"
namespace display_device {
enum class DeviceEnumerationDetail { Full };
struct EnumeratedDevice { std::string m_device_id, m_display_name, m_friendly_name; };
using EnumeratedDeviceList = std::vector<EnumeratedDevice>;
std::string toJson(const EnumeratedDeviceList&, std::nullopt_t) { return "[]"; }
}
namespace config {
struct video_t {
 enum class virtual_display_mode_e { disabled, per_client, shared };
 virtual_display_mode_e virtual_display_mode=virtual_display_mode_e::disabled;
 std::string output_name;
 bool rtx_hdr=false, runtime_rtx_hdr_override=false;
 struct { std::vector<std::string> virtual_display_outputs={"Reserved-1"}; } dd;
};
inline video_t video;
}
namespace rtsp_stream {
struct launch_session_t {
 bool virtual_display=false, virtual_display_failed=false, client_requests_virtual_display=false;
 bool virtual_display_recreated_on_demand=true, virtual_display_needs_resume_apply=true;
 bool hdr=false, force_sdr=false, prefer_sdr_10bit=false;
 std::optional<bool> client_virtual_display_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::string virtual_display_device_id="old";
 std::optional<int> virtual_display_ready_since=3;
 std::optional<bool> virtual_display_hdr_enabled=true;
 std::optional<std::string> output_name_override;
};
bool effective_hdr_requested(const launch_session_t& session) {
 return session.hdr && !session.force_sdr && !session.prefer_sdr_10bit;
}
bool hdr_capture_required(const launch_session_t& session, const config::video_t& video_config) {
 return video::hdr_stream_policy::requires_hdr_source(
   effective_hdr_requested(session), video_config.runtime_rtx_hdr_override && video_config.rtx_hdr);
}
}
namespace platf {
std::vector<std::string> outputs={"/dev/dri/card0:Virtual-1", "DP-1", "Reserved-1", "DP-2"};
std::vector<std::string> display_names() { return outputs; }
namespace linux_private_display {
int mutations=0;
struct prepared { bool active; std::string output_name, error; };
prepared prepare_session(rtsp_stream::launch_session_t&, bool, bool) { ++mutations; return {true,"Virtual-1",{}}; }
bool is_kernel_output(const std::string& name) { return name=="Virtual-1"; }
bool capable() { return true; } bool ready() { return true; } bool hdr_capable() { return true; }
std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) { return {}; }
bool initialize() { ++mutations; return true; }
bool apply_session(rtsp_stream::launch_session_t&) { ++mutations; return true; }
bool revert() { ++mutations; return true; }
bool reset_persistence() { ++mutations; return true; }
void schedule_revert(std::chrono::milliseconds,std::string) { ++mutations; }
void cancel_scheduled_revert() { ++mutations; }
}
}
'''
program += interface + '\n' + (root / 'src/platform/linux/session_backend.h').read_text().replace('#pragma once', '') + '\n' + source
program += r'''
int main() {
 using namespace platf;
 unsetenv("VIBEPOLLO_MACHINE_HOST");
 setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
 assert(!linux_display::hyprland_kms_session());
 assert(std::string(linux_display::backend().capabilities().backend_name)=="kscreen-vkms");
 setenv("VIBEPOLLO_MACHINE_HOST","1",1);
 auto& backend=linux_display::backend();
 assert(!backend.capabilities().independent_outputs);
 assert(std::string(backend.capabilities().backend_name)=="hyprland-kms-physical-sdr");
 const auto devices=backend.enumerate_devices(display_device::DeviceEnumerationDetail::Full);
 assert(devices && devices->size()==2 && devices->front().m_device_id=="DP-1");
 assert(backend.capture_target("")=="__unsupported-hyprland-output__");
 assert(backend.capture_target("DP-2")=="DP-2");
 for(const auto& name : {"0","Virtual-1","/dev/dri/card0:Virtual-1","Reserved-1","missing"})
   assert(backend.capture_target(name)=="__unsupported-hyprland-output__");
 rtsp_stream::launch_session_t session;
 assert(!backend.prepare_session(session,true,true).error.empty());
 session.output_name_override="DP-1";
 auto result=backend.prepare_session(session,true,true);
 assert(result.error.empty() && !result.owns_output && result.output_name=="DP-1");
 assert(session.output_name_override=="DP-1" && session.virtual_display_device_id.empty());
 assert(!session.virtual_display_ready_since && !session.virtual_display_hdr_enabled);
 assert(!session.virtual_display_needs_resume_apply);
 config::video.output_name="DP-2"; session={};
 assert(backend.prepare_session(session,true,true).output_name=="DP-2");
 config::video.output_name.clear();
 assert(backend.initialize() && backend.apply_session(session) && backend.revert());
 backend.schedule_revert({},"disconnect"); backend.cancel_scheduled_revert();
 assert(!backend.reset_persistence() && !linux_private_display::mutations);
 for(int trigger=0;trigger<4;++trigger) {
   session={};
   if(trigger==0) session.virtual_display=true;
   if(trigger==1) session.client_requests_virtual_display=true;
   if(trigger==2) session.virtual_display_mode_override=config::video_t::virtual_display_mode_e::shared;
   if(trigger==3) session.hdr=true;
   assert(!backend.prepare_session(session,true,true).error.empty());
 }
 config::video.rtx_hdr=true;
 session={}; session.hdr=true; session.output_name_override="DP-1";
 assert(!backend.prepare_session(session,true,true).error.empty());
 config::video.runtime_rtx_hdr_override=true;
 session={}; session.hdr=true; session.output_name_override="DP-1";
 assert(backend.prepare_session(session,true,true).error.empty());
 assert(!session.virtual_display_hdr_enabled);
 config::video.rtx_hdr=false;
 session={}; session.output_name_override="missing";
 assert(!backend.prepare_session(session,true,true).error.empty());
 outputs={"/dev/dri/card0:Virtual-1"}; session={};
 assert(!backend.prepare_session(session,true,true).error.empty());
 assert(backend.capture_target("")=="__unsupported-hyprland-output__");
 setenv("VIBEPOLLO_SESSION_BACKEND","kwin",1);
 assert(linux_display::backend().capabilities().independent_outputs);
 assert(linux_display::backend().initialize() && linux_private_display::mutations==1);
 std::cout << "PASS: production Hyprland physical output selection, unsupported requests, no topology mutation and KWin fallback\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'backend.cpp').write_text(program)
    subprocess.run([compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(path / 'backend.cpp'), '-o', str(path / 'backend')], check=True)
    subprocess.run([str(path / 'backend')], check=True)

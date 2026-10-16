"""Exercise the Windows limiter lifecycle with fake driver/RTSS boundaries.

Run with Python and g++ on Linux; the production orchestrator and public
headers are compiled unchanged. No NVIDIA profile or RTSS process is touched.
An optional repository path and source override allow before/after checks.
"""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory(prefix="frame-limiter-vsync-test-") as directory:
    directory_path = pathlib.Path(directory)

    def put(name, text):
        path = directory_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    put('src/config.h',r'''
#pragma once
#include <cstdint>
#include <string>
namespace config {
 struct limiter_t { bool enable=false; std::string provider="auto"; bool disable_vsync=false; std::uint32_t fps_limit_millihz=0; }; inline limiter_t frame_limiter;
 struct rtss_t { bool allow_virtual_display_override=false; std::string frame_limit_type="async"; }; inline rtss_t rtss;
 struct video_t { std::string capture, adapter_name, adapter_pnp_id; }; inline video_t video;
 inline std::string output;
 inline bool sync_override=false, provider_override=false;
 inline std::string get_active_output_name() { return output; }
 inline bool has_runtime_config_override(const char *key) {
  return std::string(key)=="rtss_frame_limit_type" ? sync_override : provider_override;
 }
}
''')
    put('src/logging.h',r'''
#pragma once
struct null_log { template<class T> null_log& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) null_log{}
''')
    put('src/platform/windows/misc.h',r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace platf {
 struct gpu { std::string name, pnp; std::uint32_t vendor_id; int memory; };
 inline const gpu nvidia{"NVIDIA", "nv-id", 0x10de, 8192}, amd{"AMD", "amd-id", 0x1002, 512}, intel{"Intel", "intel-id", 0x8086, 512};
 inline std::vector<gpu> gpus{nvidia};
 inline std::string output_adapter="NVIDIA", automatic_output="display";
 enum class adapter_resolution_status_e {resolved, not_found, unknown, ambiguous};
 inline auto configured_failure=adapter_resolution_status_e::resolved;
 struct adapter_resolution_t {
  adapter_resolution_status_e status=adapter_resolution_status_e::unknown;
  std::string description;
  std::uint32_t vendor_id=0;
  explicit operator bool() const { return status==adapter_resolution_status_e::resolved; }
 };
 inline std::string_view adapter_resolution_status_name(adapter_resolution_status_e) { return "test"; }
 inline std::vector<gpu> enumerate_gpus() { return gpus; }
 inline bool has_nvidia_gpu() {
  for (const auto& item:gpus) if(item.vendor_id==0x10de) return true;
  return false;
 }
 inline adapter_resolution_t resolve_adapter(std::string_view name, std::string_view pnp) {
  if (configured_failure!=adapter_resolution_status_e::resolved) return {configured_failure};
  for (const auto& item:gpus) if(pnp.empty() ? item.name==name : item.pnp==pnp)
   return {adapter_resolution_status_e::resolved,item.name,item.vendor_id};
  return {adapter_resolution_status_e::not_found};
 }
 inline adapter_resolution_t resolve_output_adapter(std::string_view output) {
  return output.empty() ? adapter_resolution_t{} : resolve_adapter(output_adapter,{});
 }
 inline adapter_resolution_t resolve_preferred_render_adapter(std::string_view, std::string_view) {
  if(gpus.empty()) return {};
  const auto& item=*std::max_element(gpus.begin(),gpus.end(),[](const auto& a,const auto& b){ return a.memory<b.memory; });
  return {adapter_resolution_status_e::resolved,item.name,item.vendor_id};
 }
}
''')
    put('src/display_device.h',r'''
#pragma once
#include <string>
namespace display_device { inline std::string map_output_name(const std::string& output) { return output; } }
''')
    put('src/platform/windows/display.h',r'''
#pragma once
#include <optional>
#include <string>
#include "src/platform/windows/misc.h"
namespace platf {
 enum class mem_type_e {dxgi};
 namespace dxgi {
  struct capture_output_identity_t { std::string output_name; };
  inline std::optional<capture_output_identity_t> resolve_automatic_capture_output(mem_type_e) {
   if(automatic_output.empty()) return std::nullopt;
   return capture_output_identity_t{automatic_output};
  }
 }
}
''')
    put('test.cpp',r'''
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "src/config.h"
#include "src/platform/windows/frame_limiter.h"
#include "src/platform/windows/frame_limiter_nvcp.h"
#include "src/platform/windows/misc.h"
namespace {
 struct call { int fps; bool limit; bool disable_limit; bool vsync; };
 std::vector<call> calls;
 int stops=0;
 bool stalled=false;
 std::string applied_sync;
 auto rtss_result=platf::rtss_apply_result::applied;
}
namespace platf {
 bool rtss_is_configured() { return true; }
 rtss_recovery_audit_result rtss_audit_pending_recovery() { return rtss_recovery_audit_result::clear; }
 rtss_apply_result rtss_streaming_start(int, int) { applied_sync=config::rtss.frame_limit_type; return rtss_result; }
 rtss_apply_result rtss_streaming_refresh(int, int) { return rtss_apply_result::safe_to_fallback; }
 bool rtss_hooks_stalled() { return stalled; }
 void rtss_streaming_stop(bool) {}
 bool rtss_warmup_process() { return true; }
 rtss_status_t rtss_get_status() { return {}; }
 namespace frame_limiter_nvcp {
  bool is_available() { return true; }
  bool streaming_start(int fps, bool limit, bool disable_limit, bool vsync, bool, bool) {
   calls.push_back({fps,limit,disable_limit,vsync}); return true;
  }
  void streaming_stop() { ++stops; }
 }
}
int main() {
 int scenarios=0;
 for (bool vsync : {false,true}) {
  for (int policy_kind=0; policy_kind<4; ++policy_kind) {
   for (const auto* provider : {"nvidia-control-panel", "rtss"}) {
    calls.clear(); stops=0; stalled=false;
    config::frame_limiter.enable=true;
    config::frame_limiter.provider=provider;
    config::frame_limiter.disable_vsync=vsync;
    config::rtss.frame_limit_type="async";
    framegen::stream_start_policy_t policy;
    policy.fps=120;
    policy.frame_generation_provider="game-provided";
    policy.frame_generation_enabled=policy_kind!=0;
    policy.uses_virtual_display=policy_kind==1;
    policy.effective_wgc_capture=policy_kind==1;
    policy.auto_virtual_framegen_limiter=policy_kind==1;
    policy.physical_framegen_capture=policy_kind==2;
    policy.capture_fix_enabled=policy_kind==3;
    platf::frame_limiter_streaming_start(platf::frame_limiter_owner::rtsp,policy);
    assert(config::frame_limiter.disable_vsync==vsync);
    assert(platf::frame_limiter_get_status().disable_vsync==vsync);
    assert(calls.size()==1);
    assert(calls.back().vsync==vsync);
    assert(calls.back().fps==120);
    if (std::string(provider)=="rtss") {
     assert(calls.back().disable_limit && !calls.back().limit);
     stalled=true;
     platf::frame_limiter_streaming_refresh();
     assert(calls.size()==2 && calls.back().vsync==vsync);
     assert(calls.back().limit && !calls.back().disable_limit);
    } else {
     assert(calls.back().limit && !calls.back().disable_limit);
    }
    const auto call_count=calls.size();
    platf::frame_limiter_streaming_start(platf::frame_limiter_owner::webrtc,policy);
    assert(calls.size()==call_count);
    const auto stop_count=stops;
    platf::frame_limiter_streaming_stop(platf::frame_limiter_owner::rtsp);
    assert(stops==stop_count);
    platf::frame_limiter_streaming_stop(platf::frame_limiter_owner::webrtc);
    assert(stops==stop_count+1);
    assert(config::frame_limiter.enable);
    assert(config::frame_limiter.provider==provider);
    assert(config::frame_limiter.disable_vsync==vsync);
    assert(config::rtss.frame_limit_type=="async");
    ++scenarios;
   }
  }
 }
 std::cout << scenarios << " frame limiter VSYNC lifecycle scenarios passed\n";

 using namespace platf;
 int adapter_scenarios=0;
 auto check=[&](const framegen::stream_start_policy_t& policy, const std::string& expected,
                const std::string& provider="auto", bool sync_override=false,
                bool virtual_override=false, const std::string& initial_sync="async") {
  calls.clear(); applied_sync.clear();
  config::frame_limiter.enable=false;
  config::frame_limiter.provider=provider;
  config::frame_limiter.disable_vsync=false;
  config::rtss.frame_limit_type=initial_sync;
  config::rtss.allow_virtual_display_override=virtual_override;
  config::sync_override=sync_override;
  const auto owner=adapter_scenarios%2 ? frame_limiter_owner::webrtc : frame_limiter_owner::rtsp;
  frame_limiter_streaming_start(owner,policy);
  if(provider=="none" || provider=="nvidia-control-panel") {
   assert(applied_sync.empty());
   assert(config::rtss.frame_limit_type==initial_sync);
  } else if(applied_sync!=expected) {
   std::cerr << "sync mismatch in scenario " << adapter_scenarios << ": " << applied_sync << " != " << expected
             << "; selected name=" << config::video.adapter_name << " pnp=" << config::video.adapter_pnp_id << '\n';
   std::abort();
  }
  frame_limiter_streaming_stop(owner);
  assert(!config::frame_limiter.enable);
  assert(config::frame_limiter.provider==provider);
  assert(config::rtss.frame_limit_type==initial_sync);
  ++adapter_scenarios;
 };
 for (const auto& inventory:std::vector<std::vector<gpu>>{
       {nvidia},{amd},{intel},{amd,nvidia},{nvidia,amd},{intel,nvidia},{nvidia,intel}}) {
  for(const auto& selected:inventory) {
   gpus=inventory;
   // Make the selected virtual render GPU unambiguous in either enumeration order.
   for(auto& item:gpus) item.memory=item.name==selected.name ? 16384 : 512;
   output_adapter=selected.name;
   for(int route=0;route<7;++route) {
    config::video={}; config::output.clear();
    if(route==0) config::video.adapter_name=selected.name;
    if(route==1) { config::video.adapter_name="stale name"; config::video.adapter_pnp_id=selected.pnp; }
    if(route==2 || route==4) config::output="display";
    if(route==6) { config::output="unpublished"; output_adapter="missing"; }
    else output_adapter=selected.name;
    for(int kind=0;kind<4;++kind) {
     framegen::stream_start_policy_t policy;
     policy.fps=120;
     policy.uses_virtual_display=route>=4;
     policy.auto_virtual_framegen_limiter=policy.uses_virtual_display;
     policy.physical_framegen_capture=!policy.uses_virtual_display;
     policy.frame_generation_provider=kind==1 ? "game-provided" : "lossless-scaling";
     policy.smooth_motion=kind==2;
     const bool reflex=selected.vendor_id==0x10de && !policy.smooth_motion && (policy.auto_virtual_framegen_limiter || kind==1);
     check(policy,reflex ? "nvidia reflex" : "front edge sync");
     // App/client selections are stronger than every automatic policy, including Smooth Motion.
     for(const auto* mode:{"", "async", "back edge sync", "nvidia reflex", "reflex"})
      check(policy,mode,"rtss",true,false,mode);
     // A global virtual-display opt-in preserves the selected mode except for the existing game-provided policy.
     if(policy.uses_virtual_display && kind!=1)
      for(const auto* mode:{"", "async", "back edge sync", "nvidia reflex"})
       check(policy,mode,"rtss",false,true,mode);
     check(policy,"","none");
     check(policy,"","nvidia-control-panel");
    }
   }
  }
 }
 // A missing/ambiguous configured identity must not fall through to an available NVIDIA GPU.
 gpus={amd,nvidia}; config::video={}; config::video.adapter_pnp_id="missing";
 config::output="display"; output_adapter="NVIDIA";
 framegen::stream_start_policy_t policy;
 policy.fps=120; policy.uses_virtual_display=true; policy.auto_virtual_framegen_limiter=true;
 for(const auto failure:{adapter_resolution_status_e::not_found,adapter_resolution_status_e::ambiguous,adapter_resolution_status_e::unknown}) {
  configured_failure=failure;
  check(policy,"front edge sync");
  check(policy,"nvidia reflex","rtss",true,false,"nvidia reflex");
 }
 configured_failure=adapter_resolution_status_e::resolved;
 config::video={}; config::output.clear(); gpus.clear(); automatic_output.clear();
 check(policy,"front edge sync");
 // RTSS's safe fallback keeps the selected sync policy and restores it on stop.
 gpus={amd,nvidia}; config::video.adapter_pnp_id=nvidia.pnp;
 rtss_result=rtss_apply_result::safe_to_fallback;
 check(policy,"nvidia reflex");
 assert(calls.size()==1 && calls.back().limit);
 rtss_result=rtss_apply_result::retained_exclusive;
 check(policy,"nvidia reflex");
 assert(calls.size()==1 && calls.back().disable_limit);
 std::cout << adapter_scenarios << " selected-adapter and explicit-option lifecycle scenarios passed\n";
}
''')
    source = (
        pathlib.Path(sys.argv[2])
        if len(sys.argv) > 2
        else root / "src/platform/windows/frame_limiter.cpp"
    )
    subprocess.run(
        [
            "g++", "-std=c++20", "-D_WIN32", "-pthread",
            "-I" + str(directory_path), "-I" + str(root),
            "-I" + str(root / "src/platform/windows"),
            str(source), str(directory_path / "test.cpp"),
            "-o", str(directory_path / "test"),
        ],
        check=True,
    )
    subprocess.run([str(directory_path / "test")], check=True)

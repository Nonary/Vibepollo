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
 struct video_t { std::string capture; }; inline video_t video;
 inline bool has_runtime_config_override(const char *) { return false; }
}
''')
    put('src/logging.h',r'''
#pragma once
struct null_log { template<class T> null_log& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) null_log{}
''')
    put('src/platform/windows/misc.h',r'''
#pragma once
#include <vector>
namespace platf {
 inline bool has_nvidia_gpu() { return true; }
 struct gpu { int vendor_id; };
 inline std::vector<gpu> enumerate_gpus() { return {{0x10de}}; }
}
''')
    put('test.cpp',r'''
#include <cassert>
#include <iostream>
#include <vector>
#include "src/config.h"
#include "src/platform/windows/frame_limiter.h"
#include "src/platform/windows/frame_limiter_nvcp.h"
namespace {
 struct call { int fps; bool limit; bool disable_limit; bool vsync; };
 std::vector<call> calls;
 int stops=0;
 bool stalled=false;
}
namespace platf {
 bool rtss_is_configured() { return true; }
 rtss_recovery_audit_result rtss_audit_pending_recovery() { return rtss_recovery_audit_result::clear; }
 rtss_apply_result rtss_streaming_start(int, int) { return rtss_apply_result::applied; }
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

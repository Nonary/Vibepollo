#!/usr/bin/env python3
"""Execute production target/key selection against inert, counted Windows boundaries.

No Windows device is opened. The old eager key path is compiled as a negative
control and must fail the same no-capture-on-cache-lookup assertions.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'src/video.cpp').read_text()


def definition(marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


prefix = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include "src/video_encoder_probe_policy.h"
#ifndef _WIN32
#define _WIN32 1
#endif
struct LUID { unsigned long LowPart; long HighPart; };
namespace platf {
struct adapter_id_t { long high_part; unsigned long low_part; bool operator==(const adapter_id_t &) const = default; };
enum class mem_type_e { dxgi };
struct resolution_t {
  std::optional<LUID> luid;
  int status = 0;
  explicit operator bool() const { return luid.has_value(); }
};
static resolution_t configured, mapped, preferred;
inline resolution_t resolve_adapter(const std::string &, const std::string &) { return configured; }
inline resolution_t resolve_output_adapter(const std::string &) { return mapped; }
inline resolution_t resolve_preferred_render_adapter(const std::string &, const std::string &) { return preferred; }
inline bool adapter_luid_equal(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
inline const char *adapter_resolution_status_name(int) { return "missing"; }
namespace dxgi {
struct wgc_adapter_identity_t { LUID luid; std::string output_name; };
struct capture_output_identity_t { std::string output_name; adapter_id_t adapter_id; };
static std::optional<wgc_adapter_identity_t> wgc;
static int capture_tests = 0;
static std::optional<capture_output_identity_t> automatic;
inline auto get_last_wgc_adapter_identity() { return wgc; }
inline std::optional<capture_output_identity_t> resolve_automatic_capture_output(
    mem_type_e, const std::optional<adapter_id_t> &required = std::nullopt) {
  ++capture_tests;
  if (automatic && (!required || *required == automatic->adapter_id)) return automatic;
  return std::nullopt;
}
}
}
namespace boost { inline bool iequals(const std::string &a, const std::string &b) { return a == b; } }
namespace config {
static std::string active_output;
inline std::string get_active_output_name() { return active_output; }
struct config_t {
  std::string encoder, adapter_name, adapter_pnp_id;
  int hevc_mode = 0, av1_mode = 0;
  bool pyrowave = false;
  struct {
    int amd_coder = 0, amd_ltr_frames = 0, amd_input_queue_size = 0, amd_av1_tiles = 0;
    std::optional<int> amd_usage_h264, amd_usage_hevc, amd_usage_av1,
      amd_rc_h264, amd_rc_hevc, amd_rc_av1, amd_quality_h264, amd_quality_hevc,
      amd_quality_av1, amd_qvbr_quality_level, amd_vbaq, amd_preanalysis,
      amd_enforce_hrd, amd_lowlatency_mode, amd_high_motion_quality_boost,
      amd_smart_access_video, amd_av1_screen_content, amd_av1_latency_mode;
  } amd;
} video;
}
namespace display_device {
static bool active = false;
inline std::string map_output_name(const std::string &s) { return s; }
inline bool output_is_active(const std::string &) { return active; }
}
struct gate_t { bool quarantined = false; bool is_quarantined() const { return quarantined; } } native_amf_lifecycle_gate;
struct state_t {
  std::mutex mutex;
  std::optional<LUID> pending_virtual_display_adapter_hint;
  bool pending_virtual_display_adapter_hint_ready_for_verification = false;
};
static state_t state;
state_t &encoder_probe_cache_state() { return state; }
'''
production = '\n'.join([
    definition('struct probe_cache_key_t') + ';',
    definition('struct probe_adapter_identity_t') + ';',
    definition('struct probe_target_t') + ';',
    definition('platf::adapter_id_t adapter_id_from_luid'),
    definition('std::string adapter_cache_identity'),
    definition('std::string luid_cache_identity'),
    definition('probe_target_t resolve_probe_target('),
    definition('probe_cache_key_t build_probe_cache_key('),
])
main = r'''
static const LUID a {1, 0}, b {2, 0};
static void reset() {
  config::video = {};
  config::active_output.clear();
  platf::configured = {}; platf::mapped = {}; platf::preferred = {};
  platf::dxgi::wgc.reset();
  platf::dxgi::automatic = platf::dxgi::capture_output_identity_t {"A", {0, 1}};
  platf::dxgi::capture_tests = 0;
  display_device::active = false;
  state.pending_virtual_display_adapter_hint.reset();
  state.pending_virtual_display_adapter_hint_ready_for_verification = false;
  native_amf_lifecycle_gate.quarantined = false;
}
int main() {
  reset();
  state.pending_virtual_display_adapter_hint = a;
  const auto cached = build_probe_cache_key();
  for (int i = 0; i < 100; ++i) assert(build_probe_cache_key() == cached);
  assert(cached.adapter_identity == "luid=0:1" && cached.adapter_identity_resolved);
  assert(platf::dxgi::capture_tests == 0);

  // A real probe still verifies capture selection, never the lookup-only target.
  const auto target = resolve_probe_target();
  assert(target.display_name == "A" && target.required_adapter == (platf::adapter_id_t {0, 1}));
  assert(platf::dxgi::capture_tests == 1);
  assert(build_probe_cache_key(&target) == cached);
  assert(platf::dxgi::capture_tests == 1);

  // A different hint cannot reuse capabilities, even with the old output present.
  state.pending_virtual_display_adapter_hint = b;
  assert(!(build_probe_cache_key() == cached));
  assert(platf::dxgi::capture_tests == 1);
  // Identity is only an expectation; actual observed-probe mismatch stays rejected.
  using namespace video::encoder_probe_policy;
  assert(!own_successful_cache_key({"", "luid=0:2", true}, {"luid=0:2", "luid=0:1"}));
  assert(!own_successful_cache_key({"", "luid=0:2", true}, {"luid=0:2", std::nullopt}));

  // Each configuration gate is still part of the key, not a stale-global shortcut.
  state.pending_virtual_display_adapter_hint = a;
  config::video.hevc_mode = 3; assert(!(build_probe_cache_key() == cached));
  config::video.hevc_mode = 0;
  config::video.av1_mode = 3; assert(!(build_probe_cache_key() == cached));
  config::video.av1_mode = 0;
  config::video.pyrowave = true; assert(!(build_probe_cache_key() == cached));
  config::video.pyrowave = false;
  native_amf_lifecycle_gate.quarantined = true; assert(!(build_probe_cache_key() == cached));
  native_amf_lifecycle_gate.quarantined = false;
  config::video.encoder = "different"; assert(!(build_probe_cache_key() == cached));
  assert(platf::dxgi::capture_tests == 1);

  reset();
  config::video.adapter_name = "configured";
  platf::configured.luid = a;
  const auto configured_key = build_probe_cache_key();
  for (int i = 0; i < 100; ++i) assert(build_probe_cache_key() == configured_key);
  assert(platf::dxgi::capture_tests == 0);
  assert(resolve_probe_target().display_name == "A");
  assert(platf::dxgi::capture_tests == 1);
  platf::configured.luid = b;
  assert(!(build_probe_cache_key() == configured_key));
  platf::configured.luid.reset();
  assert(!build_probe_cache_key().adapter_identity_resolved);
  assert(platf::dxgi::capture_tests == 1);

  // Configured adapter remains authoritative over a stale WGC identity.
  platf::configured.luid = a;
  config::active_output = "B"; display_device::active = true;
  platf::dxgi::wgc = platf::dxgi::wgc_adapter_identity_t {b, "B"};
  assert(build_probe_cache_key().adapter_identity == "luid=0:1");
  assert(platf::dxgi::capture_tests == 1);

  // Removing the hint/configuration does not pin Automatic to a prior GPU.
  reset();
  state.pending_virtual_display_adapter_hint = a;
  const auto hinted = build_probe_cache_key();
  state.pending_virtual_display_adapter_hint.reset();
  platf::dxgi::automatic = platf::dxgi::capture_output_identity_t {"B", {0, 2}};
  assert(build_probe_cache_key().adapter_identity == "luid=0:2");
  assert(!(build_probe_cache_key() == hinted));
  assert(platf::dxgi::capture_tests == 2);
  // Existing WGC and explicit current-output identities still avoid active tests.
  config::active_output = "B"; display_device::active = true;
  platf::dxgi::wgc = platf::dxgi::wgc_adapter_identity_t {b, "B"};
  assert(build_probe_cache_key().adapter_identity == "luid=0:2");
  assert(platf::dxgi::capture_tests == 2);
  platf::dxgi::wgc.reset(); platf::mapped.luid = b;
  assert(build_probe_cache_key().adapter_identity == "luid=0:2");
  assert(platf::dxgi::capture_tests == 2);
}
'''
with tempfile.TemporaryDirectory(prefix='encoder-probe-lookup-') as temp:
    for name, code, expect_success in [
        ('production', production, True),
        ('old-eager-key', production.replace('resolve_probe_target(false).adapter_identity',
                                             'resolve_probe_target().adapter_identity'), False),
    ]:
        cpp = Path(temp) / (name + '.cpp')
        binary = Path(temp) / name
        cpp.write_text(prefix + code + main)
        flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-I', str(root)]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run([args.compiler, *flags, str(cpp), '-o', str(binary)], check=True)
        env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
        if expect_success != (result.returncode == 0):
            raise AssertionError(f'{name}: unexpected exit {result.returncode}\n{result.stderr}')
        if not expect_success and 'capture_tests == 0' not in result.stderr:
            raise AssertionError(f'negative control failed for an unrelated reason:\n{result.stderr}')
        print(f'{name}: expected outcome verified')

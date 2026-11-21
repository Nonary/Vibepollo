#!/usr/bin/env python3
"""Exercise Windows HTTP discovery and encoder selection with inert driver boundaries."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile


def definition(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <semaphore>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "src/thread_safe.h"
#undef __linux__
#define _WIN32
std::ostringstream log_output;
#define BOOST_LOG(level) log_output
using namespace std::literals;

namespace mail {
  std::shared_ptr<safe::signal_t> process_shutdown_event() {
    static auto event = std::make_shared<safe::signal_t>();
    return event;
  }
}
using LUID = int;
namespace platf {
  using adapter_id_t = int;
  struct adapter_resolution {
    int status = 0;
    std::string description, pnp_id;
    std::optional<LUID> luid;
    explicit operator bool() const { return luid.has_value(); }
  };
  adapter_resolution resolve_adapter(const std::string &, const std::string &) { return {}; }
  adapter_resolution resolve_preferred_render_adapter(const std::string &, const std::string &) { return {}; }
  std::string adapter_resolution_status_name(int) { return {}; }
  namespace dxgi {
    struct luid_t { int HighPart, LowPart; };
    std::optional<luid_t> get_last_wgc_adapter_luid() { return std::nullopt; }
  }
}
namespace config {
  struct video_t {
    enum class virtual_display_mode_e { disabled, enabled };
    virtual_display_mode_e virtual_display_mode = virtual_display_mode_e::disabled;
    std::string encoder, adapter_name, adapter_pnp_id;
    int hevc_mode = 0, av1_mode = 0;
  } video;
  std::string get_active_output_name() { return {}; }
}
namespace amf::lifecycle {
  struct selection { bool include_experimental = true, fail_closed = false; };
  selection encoder_selection_policy(const std::string &) { return {}; }
}
namespace VDISPLAY {
  int requests = 0, releases = 0;
  struct display_lease {
    bool owns_temporary_probe_request() const { return true; }
    bool ready_for_capture() const { return true; }
  };
  display_lease ensure_display(const std::optional<LUID> &) { ++requests; return {}; }
  void cleanup_ensure_display(display_lease &) { ++releases; }
}
int validations = 0, pyrowave_probes = 0, probe_targets = 0, key_lookups = 0;
std::function<void()> inside_validation;
int shutdown_on_lookup = 0;
namespace video {
  struct advertised_encoder_capabilities_t {
    int hevc_mode = 0, av1_mode = 0, pyrowave_mode = 0;
    std::array<bool, 3> yuv444_for_codec {};
  };
  struct encoder_t {
    enum flag_e { PASSED, DYNAMIC_RANGE, YUV444, MAX_FLAGS };
    using codec_flags_t = std::bitset<MAX_FLAGS>;
    struct test_storage_t: codec_flags_t {
      using codec_flags_t::operator=;
      void store(unsigned long bits, std::memory_order) { static_cast<codec_flags_t &>(*this) = bits; }
    };
    struct codec_t {
      std::string name;
      test_storage_t capabilities;
      bool operator[](flag_e flag) const { return capabilities[flag]; }
    } h264 {"h264"}, hevc {"hevc"}, av1 {"av1"};
    std::string_view name = "test-encoder";
    int flags = 1;
    static std::string_view from_flag(flag_e) { return {}; }
  } first, second, amdvce_experimental, amdvce_ffmpeg;
  constexpr int REF_FRAMES_INVALIDATION = 1;
  const std::vector<encoder_t *> encoders {&first, &second};
  encoder_t *chosen_encoder = nullptr;
  int active_hevc_mode = 0, active_av1_mode = 0;
  std::atomic_int active_pyrowave_mode {0};
  std::atomic_bool native_amf_probe_retry_allowed {false};
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec {};
  struct { bool is_quarantined() const { return false; } } native_amf_lifecycle_gate;
  std::string adapter_cache_identity(int adapter) { return std::to_string(adapter); }
  bool allow_encoder_probing() { return true; }
  using encoder_probe_adapter_hint_lease_t = int;
  encoder_probe_adapter_hint_lease_t set_pending_virtual_display_adapter_hint(LUID) { return 1; }
  bool clear_pending_virtual_display_adapter_hint(encoder_probe_adapter_hint_lease_t) { return true; }
  int probe_encoders();
  advertised_encoder_capabilities_t advertised_encoder_capabilities(bool, bool * = nullptr);

  // The same driver stub supports the pre-change entry points for a negative control.
  template<class... Extra>
  bool validate_encoder(encoder_t &encoder, bool, const std::optional<int> &, std::optional<int> *observed,
                        const std::string &, Extra...) {
    ++validations;
    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();
    native_amf_probe_retry_allowed.store(true);
    if (inside_validation) inside_validation();
    *observed = 7;
    return &encoder == &second;
  }
  int probe_pyrowave(const std::optional<int> &) { ++pyrowave_probes; return 2; }
"""

CACHE_LOOKUP = r"""
  probe_target_t resolve_probe_target(bool = true, safe::signal_t * = nullptr) {
    ++probe_targets;
    return {.required_adapter = 7};
  }
  std::string current_adapter = "7";
  probe_cache_key_t build_probe_cache_key(const probe_target_t *target = nullptr, safe::signal_t * = nullptr) {
    if (!target && ++key_lookups == shutdown_on_lookup) {
      mail::process_shutdown_event()->raise(true);
    }
    return {
      .encoder_configuration = "test",
      .adapter_identity = current_adapter,
      .adapter_identity_source = "test",
      .adapter_identity_resolved = true,
    };
  }
"""

STAGED_VALIDATION = r"""
  bool validate_encoder(encoder_t &encoder, bool, const std::optional<int> &,
                        std::optional<int> *observed, const std::string &, int, int, safe::signal_t *,
                        encoder_probe_flags_t &flags, bool &amf_retry) {
    ++validations;
    flags.h264.set();
    flags.hevc.set();
    flags.av1.set();
    amf_retry = true;
    if (inside_validation) inside_validation();
    *observed = 7;
    return &encoder == &second;
  }
"""

HTTP_STUBS = r"""
}
struct http_encoder_capabilities_t {
  video::advertised_encoder_capabilities_t advertised;
  bool probe_complete;
};
void log_http_encoder_capabilities(const video::advertised_encoder_capabilities_t &, bool,
                                   std::string_view, std::string_view) {}
std::mutex &stream_lifecycle_mutex() { static std::mutex mutex; return mutex; }
bool has_stream_session_activity() { return false; }
bool has_active_or_stopping_stream_session() { return false; }
"""

CHECKS = r"""
void require(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main(int argc, char **argv) {
  using namespace video;
  require(argc == 2, "expected scenario name");
  const std::string_view scenario = argv[1];
  const auto event = mail::process_shutdown_event();
  if (scenario == "normal") {
    const auto reply = advertised_encoder_capabilities_for_http();
    require(reply.probe_complete && reply.advertised.hevc_mode == 3 &&
            reply.advertised.av1_mode == 3 && reply.advertised.pyrowave_mode == 2,
            "normal discovery must return completed capabilities");
    require(validations == 2 && pyrowave_probes == 1, "normal discovery must try the next encoder");
    require(advertised_encoder_capabilities_for_http().probe_complete && validations == 2,
            "a completed cache hit must avoid another probe");
    require(VDISPLAY::requests == VDISPLAY::releases, "temporary display lease must be returned");
  } else if (scenario.starts_with("route-")) {
    const auto discover = scenario == "route-windows-serverinfo" ? windows_serverinfo :
                          scenario == "route-windows-applist" ? windows_applist :
                          scenario == "route-other-serverinfo" ? other_serverinfo : other_applist;
    require(advertised_encoder_capabilities_for_http().probe_complete, "seed a positive adapter cache");
    const int before = validations;
    const int previous_targets = probe_targets;
    const int temporary_displays = VDISPLAY::requests;
    event->raise(true);
    const auto cached = discover();
    require(cached.hevc_mode == 3 && cached.av1_mode == 3 && cached.pyrowave_mode == 2 &&
            std::ranges::all_of(cached.yuv444_for_codec, [](bool value) { return value; }),
            "discovery route must preserve all capabilities on a matching cache hit");
    current_adapter = "changed-adapter";
    const auto miss = discover();
    require(miss.hevc_mode == 0 && miss.av1_mode == 0 && miss.pyrowave_mode == 0 &&
            std::ranges::none_of(miss.yuv444_for_codec, [](bool value) { return value; }),
            "discovery route must use the existing fallback for an adapter mismatch");
    require(validations == before && probe_targets == previous_targets && VDISPLAY::requests == temporary_displays,
            "discovery route must avoid a new probe on shutdown");
  } else if (scenario == "queued") {
    // Simulate work already posted to the discovery pool before the host starts
    // stopping. Display restore stays blocked until after the response returns.
    std::binary_semaphore request_queued {0}, begin_restore {0}, restore_done {0};
    std::jthread restore([&] {
      request_queued.acquire();
      event->raise(true);
      begin_restore.release();
      restore_done.acquire();
    });
    request_queued.release();
    begin_restore.acquire();
    const auto reply = advertised_encoder_capabilities_for_http();
    restore_done.release();
    require(!reply.probe_complete && reply.advertised.hevc_mode == 0,
            "queued discovery must return the existing unknown capability fallback during restore");
    require(probe_targets == 0 && validations == 0 && VDISPLAY::requests == 0,
            "queued discovery must not start display or encoder work during restore");
  } else if (scenario == "after-gate") {
    shutdown_on_lookup = 2;
    const auto reply = advertised_encoder_capabilities_for_http();
    require(!reply.probe_complete && probe_targets == 0 && VDISPLAY::requests == 0,
            "shutdown during the lifecycle-gate cache lookup must prevent new probe work");
  } else if (scenario == "in-flight") {
    require(advertised_encoder_capabilities_for_http().probe_complete, "seed a positive cache");
    const auto completed = chosen_encoder;
    const auto completed_caps = last_encoder_probe_supported_yuv444_for_codec;
    first.h264.capabilities.reset();
    native_amf_probe_retry_allowed.store(false);
    current_adapter = "changed-adapter";
    const auto attempted_key = encoder_probe_cache_state().attempted_cache_key;
    const auto before = validations;
    bool flags_visible_during_validation = false;
    inside_validation = [&] {
      flags_visible_during_validation = first.h264.capabilities.any() || native_amf_probe_retry_allowed.load();
      event->raise(true);
    };
    const auto reply = advertised_encoder_capabilities_for_http();
    require(!reply.probe_complete && reply.advertised.hevc_mode == 0,
            "an interrupted probe must not publish or reuse an unrelated adapter's capabilities");
    require(validations == before + 1 && pyrowave_probes == 1, "shutdown must stop before the next candidate");
    require(!flags_visible_during_validation, "tentative codec and AMF flags must remain private");
    require(chosen_encoder == completed && active_hevc_mode == 3 && active_av1_mode == 3 &&
            last_encoder_probe_supported_yuv444_for_codec == completed_caps &&
            first.h264.capabilities.none() && !native_amf_probe_retry_allowed.load(),
            "cancelled validation must preserve completed encoder state");
    require(encoder_probe_cache_state().attempted_cache_key == attempted_key,
            "cancelled validation must preserve completed attempt bookkeeping");
    current_adapter = "7";
    const auto cached = advertised_encoder_capabilities_for_http();
    require(cached.probe_complete && cached.advertised.hevc_mode == 3 &&
            cached.advertised.av1_mode == 3 && cached.advertised.pyrowave_mode == 2 &&
            validations == before + 1,
            "the fully verified matching snapshot must still be returned during shutdown");
    require(VDISPLAY::requests == VDISPLAY::releases, "cancelled discovery must return its temporary display lease");
  } else {
    require(false, "unknown scenario");
  }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler")
    parser.add_argument("--include-dirs", default="", help="CMake include directory list")
    parser.add_argument("--revision", help="Use Git objects instead of the working tree for a regression control")
    args = parser.parse_args()
    root = args.repository.resolve()

    def read(path):
        if args.revision:
            return subprocess.check_output(["git", "show", f"{args.revision}:{path}"], cwd=root, text=True)
        return (root / path).read_text()

    video = read("src/video.cpp")
    http = read("src/nvhttp.cpp")
    cache_start = video.index("    struct probe_cache_key_t")
    cache_end = video.index("#ifdef _WIN32\n    platf::adapter_id_t adapter_id_from_luid", cache_start)
    cache = video[cache_start:cache_end]
    policy_start = video.index("    bool probe_cache_matches(")
    policy_end = video.index("    void log_probe_cache_miss(", policy_start)
    cache_policy = video[policy_start:policy_end]
    advertised = video[video.index("  bool has_attempted_encoder_probe()"):video.index("  void reset_display(")]
    # Skip the Windows adapter lease mutators, whose effects are stubbed above.
    advertised = advertised[advertised.index("  advertised_encoder_capabilities_t advertised_encoder_capabilities("):]
    signature = ("  static std::optional<int> probe_encoders_impl(safe::signal_t *shutdown);"
                 if "probe_encoders_impl(" in video else "")
    probe = definition(video, "  static std::optional<int> probe_encoders_impl(safe::signal_t *shutdown) {"
                       if signature else "  int probe_encoders() {")
    if signature:
        probe += "\n" + definition(video, "  int probe_encoders() {")
    status = video[video.index("    struct encoder_probe_status_t"):video.index("  bool has_attempted_encoder_probe()")]
    status = "  namespace {\n" + status

    staged = ""
    if "struct encoder_probe_flags_t" in video:
        staged = definition(video, "  struct encoder_probe_flags_t") + ";\n" + STAGED_VALIDATION

    routes = []
    for route in ("serverinfo", "applist"):
        body = definition(http, f"  void {route}(")
        branches = re.search(
            r"#ifdef _WIN32\s+(const auto advertised_video = [^;]+;)\s+#else\s+"
            r"(const auto advertised_video = [^;]+;)\s+#endif",
            body,
        )
        if not branches:
            raise AssertionError(f"Could not find capability lookup in {route}")
        for platform, lookup in zip(("windows", "other"), branches.groups(), strict=True):
            routes.append(f"auto {platform}_{route}() {{ {lookup} return advertised_video; }}")

    with tempfile.TemporaryDirectory(prefix="discovery-shutdown-") as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text("\n".join((
            '#include "src/video_encoder_probe_policy.h"',
            PREFIX, cache, CACHE_LOOKUP, cache_policy,
            "  void log_probe_cache_miss(const probe_cache_key_t &) {}",
            "  std::atomic<std::int64_t> last_negative_hdr_advertisement_probe_ns {0};",
            "  std::mutex encoder_probe_mutex;",
            status, signature,
            "  advertised_encoder_capabilities_t advertised_encoder_capabilities(bool, bool *, std::string *);",
            advertised, staged, probe, HTTP_STUBS,
            definition(http, "    http_encoder_capabilities_t advertised_encoder_capabilities_for_http() {"),
            *routes, CHECKS,
        )))
        includes = ["-I" + path for path in args.include_dirs.split(";") if path]
        subprocess.run(
            [args.compiler, "-std=c++23", "-pthread", "-I", str(root), *includes, str(cpp), "-o", str(binary)],
            check=True,
        )
        failures = []
        for scenario in (
            "normal", "queued", "after-gate", "in-flight",
            "route-windows-serverinfo", "route-windows-applist",
            "route-other-serverinfo", "route-other-applist",
        ):
            result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=20)
            if result.returncode:
                failures.append(scenario)
                diagnostic = (result.stderr or result.stdout).strip().splitlines()
                print(f"{scenario}: {diagnostic[-1] if diagnostic else result.returncode}", flush=True)
            else:
                print(f"{scenario}: passed", flush=True)
        if failures:
            raise SystemExit(1)


if __name__ == "__main__":
    main()

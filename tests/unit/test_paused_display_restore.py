"""Execute the production Windows finalizer with recording platform collaborators.

The actual capture-owner predicate and finalizer are compiled. Windows IPC,
app/config state, and topology ownership are controlled boundaries. Coordinator
role/refcount behavior is separately exercised by test_remote_display_topology.
--stream-source permits a before-fix negative control without changing files.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("--stream-source", type=Path)
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()


def function(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(signature)
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    branches = []
    while depth:
        # Alternate platform branches may each open the same enclosing block.
        # Count one branch's braces while retaining both for the C++ compiler.
        if source[end] == "#" and not source[source.rfind("\n", 0, end) + 1:end].strip():
            line_end = source.find("\n", end)
            directive = source[end:line_end].split()[0]
            if directive in ("#if", "#ifdef", "#ifndef"):
                branches.append([depth, None])
            elif directive in ("#else", "#elif"):
                branches[-1][1] = depth
                depth = branches[-1][0]
            elif directive == "#endif":
                _, alternate_depth = branches.pop()
                if alternate_depth is not None and alternate_depth != depth:
                    raise ValueError("Unbalanced alternate platform branches")
            end = line_end + 1
            continue
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = (args.stream_source or args.repo / "src/stream.cpp").read_text()
functions = "\n".join(function(source, signature) for signature in [
    "bool has_capture_runtime_owner(const shared_runtime_finalize_context_t &context)",
    "void start_shared_platform_if_needed()",
    "bool finalize_shared_runtime_if_idle(",
])
retry = function((args.repo / "src/nvhttp.cpp").read_text(), "void cleanup_virtual_display_if_idle_locked()")
program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/display_target_lease.h"
// Load the actual admission guard before selecting the fixture platform.
#include "src/host_stream_control.h"
#ifndef _WIN32
#define _WIN32 1
#endif
#undef __linux__
using namespace std::chrono_literals;
struct Log { template<class T> Log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) Log{}
using GUID = std::array<std::uint8_t, 16>;
struct Uuid { std::uint8_t b8[16] {}; };
namespace fake {
  int dispatches = 0, removals = 0, platform_starts = 0, platform_stops = 0;
  int config_applies = 0, pending_clears = 0;
  bool dispatch_ok = true, rtsp_pending = false, webrtc_active = false, webrtc_capture = false;
  unsigned rtsp_sessions = 0, webrtc_teardown = 0;
}
namespace config {
  struct DD { bool remove_virtual_display_after_restore = false; bool config_revert_on_disconnect = true; int paused_virtual_display_timeout_secs = 0; std::chrono::milliseconds config_revert_delay {}; };
  struct Video { DD dd; } video;
  void set_runtime_output_name_override(std::optional<std::string>) {}
  void clear_runtime_config_overrides() {}
  void apply_config_now() { ++fake::config_applies; }
  void maybe_apply_deferred() {}
}
namespace proc {
  struct Proc { int app_id = 1; int current_app_id() const { return app_id; } } proc;
  bool consume_deferred_display_revert() { return false; }
}
namespace rtsp_stream {
  bool has_pending_launch_or_startup() { return fake::rtsp_pending; }
  unsigned session_count_no_cleanup() { return fake::rtsp_sessions; }
}
namespace webrtc_stream {
  unsigned teardown_session_count() { return fake::webrtc_teardown; }
  bool has_active_or_pending_sessions() { return fake::webrtc_active; }
  bool has_capture_active() { return fake::webrtc_capture; }
}
namespace remote_display_topology {
  struct Topology {
    unsigned identities = 1;
    bool restore_allowed = true;
    void release_drained_normal_game_identities() {}
    unsigned managed_client_identity_count() const { return identities; }
    bool physical_display_restore_allowed() const { return restore_allowed; }
    bool generic_virtual_display_cleanup_allowed() const { return identities == 0; }
    std::vector<std::string> idle_normal_game_recovery_client_ids(bool) { return {}; }
  } topology;
  Topology &instance() { return topology; }
}
namespace VDISPLAY {
  Uuid virtualDisplayUuidFromStableId(const std::string &) { return {}; }
  void cancel_virtual_display_recovery_monitor(GUID) {}
  void cancel_all_virtual_display_recovery_monitors() {}
  void restorePhysicalHdrProfiles() {}
}
namespace display_helper_integration {
  enum class RevertOwnershipPolicy { AllOwnersMustRelease, RetainPausedNormalGame };
  enum class FinalRestoreDisplayPolicy { Retain, RemoveAfterRestore };
  FinalRestoreDisplayPolicy last_retention_policy = FinalRestoreDisplayPolicy::Retain;
  unsigned cleanup_cancellations = 0;
  void cancel_final_restore_display_removal() { ++cleanup_cancellations; }
  void clear_pending_apply() { ++fake::pending_clears; }
  bool revert(bool, bool = false, bool = false, void * = nullptr,
              RevertOwnershipPolicy policy = RevertOwnershipPolicy::AllOwnersMustRelease,
              FinalRestoreDisplayPolicy retention = FinalRestoreDisplayPolicy::Retain) {
    last_retention_policy = retention;
    ++fake::dispatches;
    assert(remote_display_topology::topology.identities == 0 ||
           policy == RevertOwnershipPolicy::RetainPausedNormalGame);
    return fake::dispatch_ok;
  }
}
namespace nvhttp { void reconcile_remote_monitor_owners() {} }
namespace platf {
  void streaming_will_start() { ++fake::platform_starts; }
  void streaming_will_stop() { ++fake::platform_stops; }
  void rtss_set_sync_limiter_override(std::optional<int>) {}
  namespace virtual_display_cleanup {
    enum class revert_order_t { remove_before_restore };
    constexpr bool idle_stream_disengages_recovery_monitor(bool) { return true; }
    int idle_stream_cleanup_recovery_policy() { return 0; }
    bool run(std::string_view, bool, revert_order_t, bool,
             std::optional<std::array<std::uint8_t,16>>, int) { ++fake::removals; return true; }
  }
}
namespace stream::session {
  struct shared_runtime_finalize_context_t {
    bool ignore_current_rtsp_teardown = false, ignore_current_webrtc_teardown = false;
    bool apply_deferred_config = true, force_display_revert_when_idle = false;
    std::optional<std::array<std::uint8_t,16>> virtual_display_guid_bytes;
  };
  std::atomic_uint running_sessions {}, teardown_sessions {};
  bool shared_platform_started = true, shared_runtime_cleanup_armed = true;
  bool shared_runtime_force_display_revert_when_idle = false;
  bool shared_runtime_paused_restore_dispatched = false;
  std::optional<std::array<std::uint8_t,16>> shared_runtime_virtual_display_guid_bytes;
  std::atomic_uint64_t g_paused_display_cleanup_generation {};
  void arm_shared_runtime_cleanup(std::optional<std::array<std::uint8_t,16>> = {}) { shared_runtime_cleanup_armed = true; }
  void clear_deferred_stream_start_actions() {}
  void schedule_paused_display_cleanup(std::chrono::seconds, std::string_view, bool,
                                      std::optional<std::array<std::uint8_t,16>>) { ++fake::removals; }
  bool finalize_shared_runtime_if_idle(std::string_view, const shared_runtime_finalize_context_t & = {});
''' + functions + r'''
}
namespace nvhttp {
  bool has_stream_session_activity_or_display_cleanup() {
    return stream::session::has_capture_runtime_owner({});
  }
  void cleanup_virtual_display_state(bool) { ++fake::removals; }
''' + retry + r'''

}
void reset() {
  fake::dispatches = fake::removals = fake::platform_starts = fake::platform_stops = 0;
  fake::config_applies = fake::pending_clears = 0;
  fake::dispatch_ok = true; fake::rtsp_pending = fake::webrtc_active = fake::webrtc_capture = false;
  fake::rtsp_sessions = fake::webrtc_teardown = 0;
  remote_display_topology::topology = {};
  config::video = {}; proc::proc.app_id = 1;
  stream::session::running_sessions = stream::session::teardown_sessions = 0;
  stream::session::shared_platform_started = true;
  stream::session::shared_runtime_cleanup_armed = true;
  stream::session::shared_runtime_paused_restore_dispatched = false;
  stream::session::shared_runtime_force_display_revert_when_idle = false;
}
int main() {
  using namespace stream::session;
  {
    const display_target::descriptor_t target {
      display_target::platform_e::windows_device,
      display_target::kind_e::physical,
      "DISPLAY-LEASED"
    };
    const display_target::mode_t mode {1920, 1080, 60000, false};
    auto protected_target = display_target::leases().acquire(
      "paused-restore-test", 1, target, mode,
      [&](const display_target::descriptor_t &requested) -> std::optional<display_target::observation_t> {
        return display_target::observation_t {requested, mode, "\\\\.\\DISPLAY1", {}};
      });
    assert(protected_target);
    reset();
    assert(!finalize_shared_runtime_if_idle("borrowed-output", {}));
    assert(fake::dispatches == 1 && fake::removals == 0 && fake::platform_stops == 0);
    assert(display_helper_integration::last_retention_policy == display_helper_integration::FinalRestoreDisplayPolicy::Retain);
    display_target::leases().release("paused-restore-test", 1);
  }
  reset();
  config::video.dd.remove_virtual_display_after_restore = true;
  assert(!finalize_shared_runtime_if_idle("pause-retains", {}));
  assert(display_helper_integration::last_retention_policy == display_helper_integration::FinalRestoreDisplayPolicy::Retain);
  reset();
  config::video.dd.remove_virtual_display_after_restore = true;
  proc::proc.app_id = 0;
  remote_display_topology::topology.identities = 0;
  assert(finalize_shared_runtime_if_idle("final-removes", {}));
  assert(display_helper_integration::last_retention_policy == display_helper_integration::FinalRestoreDisplayPolicy::RemoveAfterRestore);
  reset();
  assert(!finalize_shared_runtime_if_idle("pause", {}));
  assert(fake::dispatches == 1); // fails on the unchanged current-master function
  assert(fake::removals == 0 && fake::platform_stops == 0 && fake::config_applies == 0);
  assert(remote_display_topology::topology.identities == 1 && shared_runtime_cleanup_armed);
  assert(!finalize_shared_runtime_if_idle("retry", {}));
  assert(fake::dispatches == 1);
  start_shared_platform_if_needed(); // Resume re-arms only the next idle boundary.
  assert(!finalize_shared_runtime_if_idle("pause-again", {}));
  assert(fake::dispatches == 2);
  reset(); fake::dispatch_ok = false;
  assert(!finalize_shared_runtime_if_idle("failed", {}));
  fake::dispatch_ok = true;
  assert(!finalize_shared_runtime_if_idle("retry", {}));
  assert(fake::dispatches == 2 && fake::removals == 0);
  reset(); fake::dispatch_ok = false;
  assert(!finalize_shared_runtime_if_idle("failed", {}));
  fake::dispatch_ok = true;
  nvhttp::cleanup_virtual_display_if_idle_locked();
  assert(fake::dispatches == 2 && fake::removals == 0);
  nvhttp::cleanup_virtual_display_if_idle_locked();
  assert(fake::dispatches == 2 && fake::removals == 0);
  for (int gate = 0; gate < 9; ++gate) {
    reset();
    switch (gate) {
      case 0: config::video.dd.config_revert_on_disconnect = false; break;
      case 1: remote_display_topology::topology.restore_allowed = false; break;
      case 2: fake::rtsp_pending = true; break;
      case 3: fake::rtsp_sessions = 1; break;
      case 4: running_sessions = 1; break;
      case 5: teardown_sessions = 1; break;
      case 6: fake::webrtc_active = true; break;
      case 7: fake::webrtc_capture = true; break;
      case 8: fake::webrtc_teardown = 1; break;
    }
    assert(!finalize_shared_runtime_if_idle("blocked", {}));
    assert(fake::dispatches == 0 && fake::removals == 0 && fake::platform_stops == 0);
  }
  // The ending transport may exclude its own teardown reservation, while any
  // other tearing-down owner must still block physical restoration.
  reset(); teardown_sessions = 1;
  shared_runtime_finalize_context_t context; context.ignore_current_rtsp_teardown = true;
  assert(!finalize_shared_runtime_if_idle("own-teardown", context));
  assert(fake::dispatches == 1);
  reset(); teardown_sessions = 2;
  assert(!finalize_shared_runtime_if_idle("peer-teardown", context));
  assert(fake::dispatches == 0);
  reset(); remote_display_topology::topology.identities = 0; proc::proc.app_id = 0;
  assert(finalize_shared_runtime_if_idle("terminal", {}));
  assert(fake::dispatches == 1 && fake::platform_stops == 1 && !shared_runtime_cleanup_armed);
  std::cout << "paused physical restore: production finalizer cases passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="paused-display-restore-") as tmp:
    path = Path(tmp)
    (path / "test.cpp").write_text(program)
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                    "-pthread", "-I", str(args.repo),
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

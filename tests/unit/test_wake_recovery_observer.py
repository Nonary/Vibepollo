"""Execute the production Windows incident observer with deterministic collaborators.

The observer itself is extracted unchanged except for access control. Windows
display APIs are replaced here; this checks admission and ordering, not hardware.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/platform/windows/display_helper_integration.cpp").read_text()
start = source.index("class OrphanRecoveryMonitor")
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
observer = source[start:end].replace("private:", "public:") + ";"
key_start = source.index("static std::string cleanup_target_key")
key_brace = source.index("{", key_start)
depth, key_end = 1, key_brace + 1
while depth:
    depth += (source[key_end] == "{") - (source[key_end] == "}")
    key_end += 1
cleanup_key = source[key_start:key_end]

program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include "src/platform/windows/wake_recovery_cleanup_policy.h"
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
struct GUID { std::array<std::uint8_t, 16> bytes {}; };
struct RecoveryTicket { std::uint64_t id, connection_generation; };
enum class RecoveryStatus { Unknown, Active, Failed, Restored };
struct RecoveryStatusSnapshot {
  std::uint64_t ticket, connection_generation;
  RecoveryStatus status;
  std::uint64_t event_revision;
  bool parked = false;
};
namespace fake {
  bool owner = false, physical = true, removal_succeeds = true, restore_succeeds = true;
  unsigned removals = 0, captures = 0, restores = 0, parks = 0, passive_reads = 0;
  int physical_version = 1;
  unsigned owner_mask = 0;
  std::string active_id = "physical";
  std::set<std::string> managed;
  std::deque<RecoveryStatusSnapshot> replies;
  std::function<void(unsigned)> on_capture, on_remove;
}
namespace rtsp_stream {
  unsigned session_count_no_cleanup() { return fake::owner || (fake::owner_mask & 1) ? 1 : 0; }
  bool has_pending_launch_or_startup() { return fake::owner || (fake::owner_mask & 2); }
}
namespace stream::session {
  std::atomic<unsigned> running_sessions {0}, teardown_sessions {0};
  bool has_capture_runtime_owner() { return fake::owner || (fake::owner_mask & 32); }
}
namespace webrtc_stream {
  bool has_active_or_pending_sessions() { return fake::owner || (fake::owner_mask & 4); }
  bool has_capture_active() { return fake::owner || (fake::owner_mask & 8); }
  bool has_teardown_in_progress() { return fake::owner || (fake::owner_mask & 16); }
}
namespace proc {
  struct Process { int current_app_id() { return fake::owner || (fake::owner_mask & 64) ? 1 : 0; } } proc;
}
namespace nvhttp {
  std::mutex &stream_lifecycle_mutex() { static std::mutex mutex; return mutex; }
  bool has_remote_role_owner() { return fake::owner || (fake::owner_mask & 128); }
}
namespace remote_display_topology {
  struct Topology {
    bool has_live_managed_client_identity() { return fake::owner || (fake::owner_mask & 256); }
    std::set<std::string> protected_remote_monitor_client_ids() {
      return fake::owner ? std::set<std::string>{"retained"} : std::set<std::string>{};
    }
  };
  Topology &instance() { static Topology topology; return topology; }
}
namespace VDISPLAY {
  enum class ensure_display_backend_e { none, sunshine, sudovda };
  struct TrackedDisplayCleanupTarget {
    std::array<std::uint8_t, 16> guid_bytes {};
    std::string device_id;
    ensure_display_backend_e backend {ensure_display_backend_e::none};
  };
  bool has_retained_ensure_display() { return fake::owner || (fake::owner_mask & 512); }
  bool tracked_display_cleanup_target_matches(const TrackedDisplayCleanupTarget &target) {
    return target.backend != ensure_display_backend_e::none && fake::managed.contains(target.device_id);
  }
  bool remove_tracked_display_cleanup_target(const TrackedDisplayCleanupTarget &target) {
    ++fake::removals;
    if (fake::removal_succeeds) fake::managed.erase(target.device_id);
    if (fake::on_remove) fake::on_remove(fake::removals);
    return fake::removal_succeeds;
  }
}
namespace platf::virtual_display_cleanup { bool in_progress() { return fake::owner || (fake::owner_mask & 2048); } }
std::atomic<bool> g_restore_expected {true};
std::atomic<std::uint64_t> g_restore_generation {7};
std::mutex &pending_apply_execution_mutex() { static std::mutex mutex; return mutex; }
bool has_pending_apply() { return fake::owner || (fake::owner_mask & 1024); }
std::optional<RecoveryStatusSnapshot> query_recovery_status(const RecoveryTicket &, bool park, std::chrono::milliseconds, bool receive_only = false) {
  assert(park != receive_only); // Reads wait passively; only park sends a request.
  if (receive_only) ++fake::passive_reads;
  if (park) ++fake::parks;
  if (fake::replies.empty()) return RecoveryStatusSnapshot {7, 3, RecoveryStatus::Restored, 1};
  auto reply = fake::replies.front(); fake::replies.pop_front();
  reply.parked = park && reply.status == RecoveryStatus::Failed;
  return reply;
}
struct physical_settings_snapshot_t {
  struct Settings {
    std::vector<int> m_topology {1}, m_origins {1};
    std::map<std::string, int> m_modes {{"physical", 1}};
  } settings;
  int version = 1;
};
std::optional<physical_settings_snapshot_t> capture_physical_settings_snapshot() {
  ++fake::captures;
  if (fake::on_capture) fake::on_capture(fake::captures);
  if (!fake::physical) return std::nullopt;
  physical_settings_snapshot_t result {{}, fake::physical_version};
  result.settings.m_modes = {{fake::active_id, 1}};
  return result;
}
bool physical_settings_match(const physical_settings_snapshot_t &a, const physical_settings_snapshot_t &b) {
  return a.version == b.version;
}
bool restore_physical_settings_snapshot(const physical_settings_snapshot_t &snapshot) {
  ++fake::restores;
  if (fake::restore_succeeds) fake::physical_version = snapshot.version;
  return fake::restore_succeeds;
}
''' + cleanup_key + "\n" + observer + r'''
void reset() {
  fake::owner = false; fake::physical = true; fake::removal_succeeds = true; fake::restore_succeeds = true;
  fake::removals = fake::captures = fake::restores = fake::parks = fake::passive_reads = 0;
  fake::physical_version = 1; fake::managed = {"a", "b", "c"}; fake::replies.clear();
  fake::owner_mask = 0; fake::active_id = "physical";
  fake::on_capture = {}; fake::on_remove = {};
  g_restore_expected.store(true); g_restore_generation.store(7);
}
void reply(RecoveryStatus status, std::uint64_t event = 1) { fake::replies.push_back({7, 3, status, event}); }
void run(unsigned targets = 1) {
  OrphanRecoveryMonitor monitor;
  OrphanRecoveryMonitor::incident_t incident {{7, 3}, {}, {"physical"}};
  for (unsigned index = 0; index < targets; ++index) {
    const char id = static_cast<char>('a' + index);
    VDISPLAY::TrackedDisplayCleanupTarget target;
    target.guid_bytes[0] = static_cast<std::uint8_t>(id); target.device_id = std::string(1, id);
    target.backend = VDISPLAY::ensure_display_backend_e::sunshine;
    incident.targets.push_back(target);
  }
  monitor.incident_ = incident; monitor.incident_revision_ = 1;
  monitor.run_incident(incident, 1, {});
}
int main() {
  VDISPLAY::TrackedDisplayCleanupTarget sunshine_target;
  sunshine_target.device_id = "same-output";
  sunshine_target.backend = VDISPLAY::ensure_display_backend_e::sunshine;
  auto sudovda_target = sunshine_target;
  sudovda_target.backend = VDISPLAY::ensure_display_backend_e::sudovda;
  assert(cleanup_target_key(sunshine_target) != cleanup_target_key(sudovda_target));
  reset();
  OrphanRecoveryMonitor ownership;
  for (unsigned mask = 1; mask <= 2048; mask <<= 1) {
    fake::owner_mask = mask; assert(!ownership.owners_clear_under_lifecycle());
  }
  fake::owner_mask = 0;
  stream::session::running_sessions.store(1); assert(!ownership.owners_clear_under_lifecycle());
  stream::session::running_sessions.store(0);
  stream::session::teardown_sessions.store(1); assert(!ownership.owners_clear_under_lifecycle());
  stream::session::teardown_sessions.store(0); assert(ownership.owners_clear_under_lifecycle());
  reset(); reply(RecoveryStatus::Failed, 0); run(); assert(fake::removals == 0 && fake::parks == 0);
  reset(); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed); run();
  assert(fake::removals == 1 && !fake::managed.contains("a") && fake::managed.contains("c"));
  assert(fake::passive_reads == 1 && fake::parks == 1);
  reset(); reply(RecoveryStatus::Active); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed); run();
  assert(fake::removals == 1); // A wake during active recovery survives until known failure.
  reset(); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Active); run(); assert(fake::removals == 0);
  reset(); fake::owner = true; reply(RecoveryStatus::Failed); run(); assert(fake::removals == 0 && fake::parks == 0);
  reset(); fake::physical = false; reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed); run(); assert(fake::removals == 0);
  reset(); fake::active_id = "new-unrelated-output"; reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed);
  run(); assert(fake::removals == 0);
  reset(); fake::managed.erase("a"); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed); run(); assert(fake::removals == 0);
  reset(); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed);
  fake::on_capture = [](unsigned) { g_restore_generation.store(8); }; run(); assert(fake::removals == 0);
  reset(); reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed);
  fake::on_remove = [](unsigned) { fake::owner = true; }; run(2); assert(fake::removals == 1 && fake::managed.contains("b"));
  reset(); fake::removal_succeeds = false; reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed); run();
  assert(fake::removals == 1 && fake::managed.contains("a"));
  reset(); fake::restore_succeeds = false; reply(RecoveryStatus::Failed); reply(RecoveryStatus::Failed);
  fake::on_remove = [](unsigned) { ++fake::physical_version; }; run(2);
  assert(fake::removals == 1 && fake::restores == 1 && fake::managed.contains("b"));
  reset(); reply(RecoveryStatus::Failed);
  { std::lock_guard lock(nvhttp::stream_lifecycle_mutex()); run(); } assert(fake::removals == 0);
  reset(); reply(RecoveryStatus::Failed);
  { std::lock_guard lock(pending_apply_execution_mutex()); run(); } assert(fake::removals == 0);
}
'''
with tempfile.TemporaryDirectory(prefix="wake-recovery-observer-") as directory:
    generated = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    generated.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=40)

# The incident's target admission relies on the real backend-independent
# facade; exercise its routing and fail-closed classification in this case.
subprocess.run([sys.executable, str(root / "tests/unit/test_wake_recovery_backend.py"),
                str(root), compiler], check=True)

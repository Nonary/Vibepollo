"""Compile production display-helper recovery control flow with fake collaborators.

Usage: python3 tests/unit/test_display_helper_recovery.py [repo]
       python3 tests/unit/test_display_helper_recovery.py [repo] [integration-source] [legacy-source] [ipc-source]
       append --compiler <c++ compiler> to use an explicit compiler
       append --retain-source <path> to keep the generated C++ translation unit

Source overrides allow each production side of the recovery behavior to be
checked independently against a before/after file. Windows APIs are not used.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


args = sys.argv[1:]
compiler = "g++"
retained_source = None
if "--compiler" in args:
    compiler_index = args.index("--compiler")
    compiler = args[compiler_index + 1]
    del args[compiler_index:compiler_index + 2]
if "--retain-source" in args:
    retain_index = args.index("--retain-source")
    retained_source = pathlib.Path(args[retain_index + 1])
    del args[retain_index:retain_index + 2]
root = pathlib.Path(args[0]).resolve() if args else pathlib.Path(__file__).resolve().parents[2]
integration_path = pathlib.Path(args[1]) if len(args) > 1 else root / "src/platform/windows/display_helper_integration.cpp"
legacy_path = pathlib.Path(args[2]) if len(args) > 2 else root / "tools/display_settings_helper.cpp"
ipc_path = pathlib.Path(args[3]) if len(args) > 3 else root / "src/platform/windows/ipc/display_settings_client.cpp"


def function(source, signature):
    # Formatting may wrap a production signature without changing its API.
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(f"Production function not found: {signature}")
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


integration_source = integration_path.read_text()
integration = function(integration_source, "bool revert(const bool prefer_golden_if_current_missing, const bool override_managed_ownership")
legacy = function(legacy_path.read_text(), "void attempt_revert_after_disconnect(ServiceState &state, std::atomic<bool> &running, uint64_t connection_epoch)")
legacy_source = legacy_path.read_text()
heartbeat_ping = function(legacy_source, "void record_heartbeat_ping()")
settlement_signatures = (
    "bool begin_disconnect_settlement(uint64_t connection_epoch)",
    "bool confirm_disconnect_owner(bool live_owner, uint64_t worker_epoch)",
    "bool handle_stream_owner_ping(std::span<const uint8_t> payload, uint64_t worker_epoch)",
    "bool poll_disconnect_settlement()",
)
has_settlement = all(signature in legacy_source for signature in settlement_signatures)
handle_apply = function(legacy_source, "bool handle_apply(ServiceState &state, std::span<const uint8_t> payload, std::string &error_msg)")
assert "state.stop_restore_polling();" in handle_apply, "accepted APPLY must clear any prior settlement or restore request"
handle_revert = function(legacy_source, "void handle_revert(ServiceState &state, std::atomic<bool> &running, std::span<const uint8_t> payload)")
if has_settlement:
    assert "state.clear_disconnect_settlement();" in handle_revert, "explicit REVERT must clear provisional settlement before restoring"
    handle_misc = function(legacy_source, "void handle_misc(")
    assert "state.handle_stream_owner_ping(payload, worker_epoch);" in handle_misc, "owner PING must carry its worker connection epoch"
settlement_methods = "\n".join(function(legacy_source, signature) for signature in settlement_signatures) if has_settlement else ""
settlement_wait = function(legacy_source, "int disconnect_settlement_wait_ms() const") if "int disconnect_settlement_wait_ms() const" in legacy_source else ""
preserve_signature = "bool preserve_pending_disconnect_settlement(uint64_t connection_epoch)"
has_preserve = preserve_signature in legacy_source
preserve_settlement = function(legacy_source, preserve_signature) if has_preserve else ""
clear_settlement = function(legacy_source, "void clear_disconnect_settlement()") if "void clear_disconnect_settlement()" in legacy_source else ""
stop_restore_polling = function(legacy_source, "void stop_restore_polling()")
ping_send = function(ipc_path.read_text(), "bool send_ping(std::optional<bool> stream_owner_active)")
config_source = (root / "src/config.cpp").read_text()
assert "display_helper_integration::revert(true, false, dd_was_enabled && dd_disabled_now);" in config_source, \
    "configuration disable transition must be the explicit disabled-recovery caller"
has_disabled_recovery_parameter = "allow_disabled_recovery" in integration[:integration.index("{")]
has_recovery_ticket_parameter = "RecoveryTicket" in integration[:integration.index("{")]
integration_wrapper = "" if has_disabled_recovery_parameter else "\n  bool revert(bool prefer, bool override_owner, bool) { return revert(prefer, override_owner); }\n"
ticket_declaration = "" if not has_recovery_ticket_parameter else r'''
  struct RecoveryTicket {
    std::uint64_t id = 0;
    std::uint64_t connection_generation = 0;
  };
  bool revert(bool, bool, bool, RecoveryTicket * = nullptr);
  struct FakeOrphanRecoveryMonitor {
    struct incident_t {
      RecoveryTicket ticket;
      std::vector<VDISPLAY::TrackedDisplayCleanupTarget> targets;
      std::set<std::string> known_physical_output_ids;
    };
    std::optional<incident_t> published;
    void publish(incident_t incident) { published = std::move(incident); }
  };
  FakeOrphanRecoveryMonitor &orphan_recovery_monitor() {
    static FakeOrphanRecoveryMonitor monitor;
    return monitor;
  }
'''

program = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
constexpr int info = 0, warning = 1, debug = 2;

namespace integration_fake {
  int ownership_checks = 0, cancels = 0, verification_invalidations = 0;
  int queue_clears = 0, helper_calls = 0, helper_starts = 0, revert_sends = 0, active_clears = 0;
  bool cleanup_allowed = true, helper_start_result = true, send_result = true;
  bool helper_force = false, helper_override = false, last_prefer_golden = false;
  bool helper_automation_enabled = true;
  std::mutex execution_mutex;
  std::atomic<bool> restore_expected {false};
  std::atomic<long long> last_revert {0};
  std::atomic<unsigned long long> restore_generation {0};
  void reset() {
    ownership_checks = cancels = verification_invalidations = queue_clears = 0;
    helper_calls = helper_starts = revert_sends = active_clears = 0;
    cleanup_allowed = helper_start_result = send_result = true;
    helper_force = helper_override = last_prefer_golden = false;
    helper_automation_enabled = true;
    restore_expected.store(false); last_revert.store(0); restore_generation.store(0);
  }
}
namespace config {
  struct VideoDD { bool config_revert_on_disconnect = false; };
  struct Video { VideoDD dd; };
  Video video;
}
namespace remote_display_topology {
  struct Topology {
    bool generic_virtual_display_cleanup_allowed() { ++integration_fake::ownership_checks; return integration_fake::cleanup_allowed; }
  };
  Topology &instance() { static Topology topology; return topology; }
}
namespace proc { void defer_display_revert() {} }
namespace VDISPLAY {
  enum class ensure_display_backend_e { none, sunshine, sudovda };
  struct TrackedDisplayCleanupTarget {
    std::array<std::uint8_t, 16> guid_bytes {};
    std::string device_id;
    ensure_display_backend_e backend {ensure_display_backend_e::none};
  };
  std::vector<TrackedDisplayCleanupTarget> targets;
  std::vector<TrackedDisplayCleanupTarget> tracked_display_cleanup_targets() { return targets; }
  void cancel_all_virtual_display_recovery_monitors() { ++integration_fake::cancels; }
}
namespace platf::display_helper_client {
  bool send_revert(std::string, std::uint64_t *connection_generation = nullptr) {
    ++integration_fake::revert_sends;
    if (connection_generation && integration_fake::send_result) *connection_generation = 7;
    VDISPLAY::targets.push_back({{}, "created-during-dispatch"});
    return integration_fake::send_result;
  }
}
namespace {
  std::mutex &pending_apply_execution_mutex() { return integration_fake::execution_mutex; }
  void invalidate_apply_verification() { ++integration_fake::verification_invalidations; }
  void clear_pending_apply_queue_locked() { ++integration_fake::queue_clears; }
  [[maybe_unused]] bool ensure_helper_started(bool force, bool override_ownership);
  [[maybe_unused]] bool ensure_helper_started() { return ensure_helper_started(false, false); }
  [[maybe_unused]] bool ensure_helper_started(bool force, bool override_ownership) {
    ++integration_fake::helper_calls;
    integration_fake::helper_force = force;
    integration_fake::helper_override = override_ownership;
    if (!integration_fake::helper_automation_enabled && !override_ownership) return false;
    ++integration_fake::helper_starts;
    return integration_fake::helper_start_result;
  }
  std::string build_revert_payload(bool prefer, std::optional<std::uint64_t> = std::nullopt) {
    integration_fake::last_prefer_golden = prefer; return "{}";
  }
  [[maybe_unused]] std::set<std::string> capture_known_physical_output_ids() { return {"physical"}; }
  long long now_steady_us() { return 42; }
  auto &g_restore_expected = integration_fake::restore_expected;
  auto &g_last_revert_us = integration_fake::last_revert;
  auto &g_restore_generation = integration_fake::restore_generation;
  void clear_active_session() { ++integration_fake::active_clears; }
}
namespace display_helper_integration {
''' + ticket_declaration + integration + r'''
''' + integration_wrapper + r'''
}

static void test_disabled_configuration_still_cleans_up_existing_mutation() {
  using namespace integration_fake;
  reset();
  config::video.dd.config_revert_on_disconnect = false;
  assert(!config::video.dd.config_revert_on_disconnect);
  // This public explicit cleanup has no configuration-enabled input: its behavior
  // must retire an existing mutation even when new automatic actions are disabled.
  helper_automation_enabled = false;
  assert(display_helper_integration::revert(false, false, true));
  assert(ownership_checks == 1 && cancels == 1 && verification_invalidations == 1);
  assert(queue_clears == 1 && helper_calls == 1 && helper_starts == 1 && revert_sends == 1);
#if HAS_DISABLED_RECOVERY
  assert(!helper_force && helper_override);
#endif
  assert(!last_prefer_golden);
  assert(restore_expected.load() && restore_generation.load() == 1);
  assert(active_clears == 1);
}

static void test_ordinary_disabled_revert_does_not_start_or_send() {
  using namespace integration_fake;
  reset(); config::video.dd.config_revert_on_disconnect = false;
  helper_automation_enabled = false;
  assert(!display_helper_integration::revert(true, false, false));
  assert(helper_calls == 1 && helper_starts == 0 && revert_sends == 0);
  assert(!restore_expected.load() && restore_generation.load() == 0);
}

static void test_foreign_owner_defers_without_touching_recovery() {
  using namespace integration_fake;
  reset(); cleanup_allowed = false;
  assert(!display_helper_integration::revert(true, false, false));
  assert(ownership_checks == 1 && cancels == 0 && verification_invalidations == 0);
  assert(queue_clears == 0 && helper_starts == 0 && revert_sends == 0 && active_clears == 0);
  reset(); cleanup_allowed = false;
  assert(display_helper_integration::revert(true, true, false));
  assert(ownership_checks == 1 && cancels == 1 && helper_starts == 1 && revert_sends == 1);
}

static void test_failed_helper_start_and_failed_send_never_report_success() {
  using namespace integration_fake;
  reset(); helper_start_result = false;
  assert(!display_helper_integration::revert(false, true, false));
  assert(helper_starts == 1 && revert_sends == 0 && !restore_expected.load());
  reset(); send_result = false;
  assert(!display_helper_integration::revert(false, true, false));
  assert(helper_starts == 1 && revert_sends == 1 && !restore_expected.load());
#if HAS_RECOVERY_TICKET
  // A failed dispatch consumes its identity so a delayed completion cannot
  // be attributed to a later request.
  assert(restore_generation.load() == 1);
#else
  assert(restore_generation.load() == 0);
#endif
}

#if HAS_RECOVERY_TICKET
static void test_incident_captures_targets_before_dispatch_and_rejects_failed_or_override_dispatch() {
  using namespace integration_fake;
  auto &monitor = display_helper_integration::orphan_recovery_monitor();
  reset(); VDISPLAY::targets = {{{}, "pre-existing-orphan", VDISPLAY::ensure_display_backend_e::sunshine}};
  monitor.published.reset();
  display_helper_integration::RecoveryTicket ticket;
  assert(display_helper_integration::revert(true, false, false, &ticket));
  assert(ticket.id == 1 && ticket.connection_generation == 7);
  assert(monitor.published && monitor.published->ticket.id == ticket.id);
  assert(monitor.published->targets.size() == 1);
  assert(monitor.published->targets.front().device_id == "pre-existing-orphan");
  assert(monitor.published->targets.front().backend == VDISPLAY::ensure_display_backend_e::sunshine);

  reset(); send_result = false; monitor.published.reset();
  assert(!display_helper_integration::revert(true, false, false, &ticket));
  assert(ticket.id == 0 && ticket.connection_generation == 0 && !monitor.published);

  reset(); monitor.published.reset();
  assert(display_helper_integration::revert(true, true, false, &ticket));
  assert(!monitor.published);
}
#endif

struct ServiceState {
  enum class RestoreWindow { Primary, Event };
  std::optional<int> last_cfg;
  std::atomic<bool> exit_after_revert {false}, direct_revert_bypass_grace {false};
  std::atomic<bool> restore_requested {false}, restore_on_disconnect {true};
  std::atomic<bool> retry_apply_on_topology {true};
  std::atomic<long long> last_apply_ms {0};
  std::atomic<std::uint64_t> restore_origin_epoch {0};
  std::atomic<bool> restore_poll_active {false}, restore_attempted_unconfirmed {false};
  std::atomic<bool> retry_revert_on_topology {true}, prefer_golden_if_current_missing {true};
  std::atomic<long long> restore_active_until_ms {0}, last_restore_event_ms {0};
  std::atomic<bool> restore_stage_running {false};
  std::atomic<std::uint64_t> command_worker_epoch {7};
  std::atomic<bool> event_pump_running {false};
  std::atomic<RestoreWindow> restore_active_window {RestoreWindow::Primary};
  std::atomic<bool> disconnect_settlement_pending {false};
  std::atomic<long long> disconnect_settlement_deadline_ms {0};
  std::mutex disconnect_settlement_mutex;
  std::uint64_t disconnect_settlement_origin_epoch = 0;
  std::jthread restore_poll_thread;
  struct EventPump { void stop() {} } event_pump;
  std::atomic<bool> heartbeat_monitor_active {false}, heartbeat_revert_armed {false};
  std::atomic<long long> heartbeat_revert_deadline_ms {0}, last_heartbeat_ms {0};
  bool epoch_current = true;
  std::uint64_t current_epoch = 7;
  int reapply_cancels = 0, disarms = 0, delayed_reapplies = 0;
  int grace_arms = 0, polling_starts = 0, golden_resets = 0, exit_graces = 0;
  bool preference_after_disarm = true;
  std::chrono::milliseconds last_grace {0};
  static constexpr auto kHeartbeatOptionalWindow = std::chrono::seconds(30);
  inline static long long fake_now_ms = 0;
  static long long steady_now_ms() {
    return fake_now_ms;
  }
  std::string last_disarm_reason;
  bool is_connection_epoch_current(std::uint64_t epoch) const { return epoch_current && epoch == current_epoch; }
  std::uint64_t current_connection_epoch() const { return current_epoch; }
  void cancel_delayed_reapply() { ++reapply_cancels; }
  void cancel_post_apply_tasks() {}
  void request_restore_cancel() {}
  void supersede_recovery_status() {}
  void reset_restore_backoff() {}
  void reset_pending_golden_session_fallbacks() {}
  void stop_and_join(std::jthread &, const char *) {}
  void arm_reconnect_exit_grace(const char *) { ++exit_graces; }
  void disarm_restore_requests(const char *reason) {
    ++disarms; last_disarm_reason = reason; preference_after_disarm = restore_on_disconnect.load();
    restore_requested.store(false); exit_after_revert.store(false); direct_revert_bypass_grace.store(false);
  }
  void schedule_delayed_reapply() { ++delayed_reapplies; }
  void arm_restore_grace(std::chrono::milliseconds grace, const char *) { ++grace_arms; last_grace = grace; }
  void reset_golden_restore_request_tracking() { ++golden_resets; }
  void ensure_restore_polling(RestoreWindow) {
    if (!disconnect_settlement_pending.load()) ++polling_starts;
  }
''' + heartbeat_ping + r'''
''' + clear_settlement + r'''
''' + stop_restore_polling + r'''
''' + settlement_methods + r'''
''' + settlement_wait + r'''
''' + preserve_settlement + r'''
};
constexpr std::chrono::milliseconds kApplyDisconnectGrace {5000};
namespace {
  std::atomic<bool> running {true};
}
''' + legacy + r'''

static long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void test_legacy_stale_epoch_and_no_mutation_paths() {
  ServiceState state; state.epoch_current = false;
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.reapply_cancels == 0 && state.grace_arms == 0 && state.polling_starts == 0);
  ServiceState empty;
  attempt_revert_after_disconnect(empty, running, 7);
  assert(empty.exit_graces == 1 && empty.grace_arms == 0 && empty.polling_starts == 0);
}

static void test_legacy_false_policy_disarms_without_changing_preference() {
  ServiceState state; state.last_cfg = 1; state.restore_on_disconnect.store(false);
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.disarms == 1 && state.exit_graces == 1);
  assert(!state.preference_after_disarm && !state.restore_on_disconnect.load());
  assert(state.grace_arms == 0 && state.polling_starts == 0);
}

static void test_legacy_explicit_restore_bypasses_policy_and_recent_apply_grace() {
  ServiceState state; state.last_cfg = 1; state.restore_on_disconnect.store(false);
  state.direct_revert_bypass_grace.store(true);
  state.last_apply_ms.store(now_ms());
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.disarms == 0 && state.delayed_reapplies == 0);
  assert(state.restore_requested.load() && state.exit_after_revert.load());
  assert(state.grace_arms == 1 && state.last_grace == 5000ms && state.polling_starts == 1);
}

static void test_recent_apply_uses_bounded_disconnect_settlement() {
#if HAS_SETTLEMENT
  ServiceState state; state.last_cfg = 1; state.restore_on_disconnect.store(true);
  state.last_apply_ms.store(now_ms());
  ServiceState::fake_now_ms = 1000;
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.restore_requested.load() && state.exit_after_revert.load());
  assert(state.disconnect_settlement_pending.load());
  assert(state.disconnect_settlement_deadline_ms.load() == 31000);
  assert(state.grace_arms == 0 && state.polling_starts == 0);

  const std::array<std::uint8_t, 0> empty {};
  const std::array<std::uint8_t, 1> false_owner {0};
  const std::array<std::uint8_t, 2> malformed {1, 0};
  assert(state.disconnect_settlement_wait_ms() == 15000);
  assert(!state.handle_stream_owner_ping(empty, 7));
  assert(!state.handle_stream_owner_ping(false_owner, 7));
  assert(!state.handle_stream_owner_ping(malformed, 7));
  assert(state.disconnect_settlement_pending.load() && state.restore_requested.load());

  const std::array<std::uint8_t, 1> true_owner {1};
  assert(!state.handle_stream_owner_ping(true_owner, 7)); // Old worker from disconnect origin.
  assert(state.disconnect_settlement_pending.load() && state.restore_requested.load());
  state.current_epoch = 8;
  state.command_worker_epoch.store(8);
  ServiceState::fake_now_ms = 5000;
  assert(state.handle_stream_owner_ping(true_owner, 8));
  const auto renewed_deadline = state.disconnect_settlement_deadline_ms.load();
  assert(state.disconnect_settlement_pending.load() && state.restore_requested.load());
  assert(state.exit_after_revert.load() && renewed_deadline == 35000);
  ServiceState::fake_now_ms = renewed_deadline;
  assert(state.disconnect_settlement_wait_ms() == 1);
  assert(!state.handle_stream_owner_ping(true_owner, 8)); // At deadline, a live-owner sample is stale.
  assert(!state.handle_stream_owner_ping(false_owner, 8));
  assert(state.disconnect_settlement_deadline_ms.load() == renewed_deadline);
  assert(state.disconnect_settlement_pending.load() && state.restore_requested.load());
  assert(state.poll_disconnect_settlement());
  assert(!state.disconnect_settlement_pending.load() && state.polling_starts == 1);
  assert(!state.confirm_disconnect_owner(true, 8));
  assert(state.restore_requested.load());
#else
  ServiceState state; state.last_cfg = 1; state.restore_on_disconnect.store(true);
  state.last_apply_ms.store(now_ms());
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.restore_requested.load()); // Baseline loses the restore while postponing it.
#endif
}

static void test_explicit_restore_and_stop_cannot_be_cleared_by_owner_ping() {
#if HAS_SETTLEMENT
  ServiceState explicit_restore;
  explicit_restore.restore_requested.store(true);
  explicit_restore.clear_disconnect_settlement();
  assert(!explicit_restore.disconnect_settlement_pending.load());
  assert(explicit_restore.disconnect_settlement_deadline_ms.load() == 0);
  assert(explicit_restore.direct_revert_bypass_grace.load());
  ServiceState::fake_now_ms = 4000;
  assert(!explicit_restore.confirm_disconnect_owner(true, 8));
  assert(explicit_restore.restore_requested.load());

  ServiceState stopped;
  stopped.disconnect_settlement_pending.store(true);
  stopped.disconnect_settlement_deadline_ms.store(30000);
  stopped.restore_requested.store(true);
  stopped.restore_origin_epoch.store(7);
  stopped.disconnect_settlement_origin_epoch = 7;
  stopped.stop_restore_polling();
  assert(!stopped.disconnect_settlement_pending.load());
  assert(stopped.disconnect_settlement_deadline_ms.load() == 0);
  assert(!stopped.restore_requested.load());
#else
  assert(true);
#endif
}

static void test_repeated_disconnect_preserves_original_settlement_deadline() {
#if HAS_SETTLEMENT
  ServiceState later; later.last_cfg = 1; later.restore_on_disconnect.store(true);
  later.last_apply_ms.store(now_ms());
  ServiceState::fake_now_ms = 500;
  attempt_revert_after_disconnect(later, running, 7);
  const auto first_deadline = later.disconnect_settlement_deadline_ms.load();
  assert(first_deadline == 30500 && later.restore_origin_epoch.load() == 7);

  later.current_epoch = 8;
  later.command_worker_epoch.store(8);
  const std::array<std::uint8_t, 1> false_owner {0};
  assert(!later.handle_stream_owner_ping(false_owner, 8));
  later.last_apply_ms.store(now_ms() - 6000); // Reconnect drops after the initial APPLY grace.
  ServiceState::fake_now_ms = 1000;
  attempt_revert_after_disconnect(later, running, 8);
  assert(later.disconnect_settlement_pending.load());
  assert(later.disconnect_settlement_deadline_ms.load() == first_deadline);
  assert(later.restore_origin_epoch.load() == 7 && later.polling_starts == 0);
  ServiceState::fake_now_ms = first_deadline;
  assert(later.poll_disconnect_settlement());
  assert(later.polling_starts == 1 && later.restore_requested.load());

  ServiceState recent; recent.last_cfg = 1; recent.restore_on_disconnect.store(true);
  recent.last_apply_ms.store(now_ms());
  ServiceState::fake_now_ms = 700;
  attempt_revert_after_disconnect(recent, running, 7);
  const auto recent_deadline = recent.disconnect_settlement_deadline_ms.load();
  recent.current_epoch = 8;
  recent.command_worker_epoch.store(8);
  recent.last_apply_ms.store(now_ms() - 1000); // A replacement connection drops inside APPLY grace.
  ServiceState::fake_now_ms = 1200;
  attempt_revert_after_disconnect(recent, running, 8);
  assert(recent.disconnect_settlement_deadline_ms.load() == recent_deadline);
  assert(recent.restore_origin_epoch.load() == 7 && recent.polling_starts == 0);
#else
  ServiceState state; state.last_cfg = 1; state.restore_on_disconnect.store(true);
  state.last_apply_ms.store(now_ms());
  attempt_revert_after_disconnect(state, running, 7);
  assert(state.restore_requested.load()); // Baseline discards the recent-APPLY restore.
#endif
}

namespace ping_wire_fake {
  enum class MsgType { Ping };
  struct Session {};
  std::vector<std::uint8_t> last_payload;
  Session *connected_session() { static Session session; return &session; }
  bool send_serialized(Session *, MsgType, const std::vector<std::uint8_t> &payload) { last_payload = payload; return true; }
}
namespace platf::display_helper_client {
  using ping_wire_fake::MsgType;
  using ping_wire_fake::Session;
  using ping_wire_fake::connected_session;
  using ping_wire_fake::send_serialized;
''' + ping_send + r'''
}

static void test_ping_wire_format_keeps_plain_probes_unowned() {
  using namespace ping_wire_fake;
  assert(platf::display_helper_client::send_ping(std::nullopt));
  assert(last_payload.empty());
  assert(platf::display_helper_client::send_ping(false));
  assert(last_payload.size() == 1 && last_payload[0] == 0);
  assert(platf::display_helper_client::send_ping(true));
  assert(last_payload.size() == 1 && last_payload[0] == 1);
}

static void test_ping_does_not_clear_an_explicit_restore_request() {
  ServiceState state;
  state.heartbeat_monitor_active.store(true);
  state.heartbeat_revert_armed.store(true);
  state.heartbeat_revert_deadline_ms.store(123);
  state.restore_requested.store(true);
  state.direct_revert_bypass_grace.store(true);
  state.record_heartbeat_ping();
  assert(!state.heartbeat_revert_armed.load() && state.heartbeat_revert_deadline_ms.load() == 0);
  assert(state.restore_requested.load() && state.direct_revert_bypass_grace.load());
}

int main() {
  test_disabled_configuration_still_cleans_up_existing_mutation();
  test_ordinary_disabled_revert_does_not_start_or_send();
  test_foreign_owner_defers_without_touching_recovery();
  test_failed_helper_start_and_failed_send_never_report_success();
#if HAS_RECOVERY_TICKET
  test_incident_captures_targets_before_dispatch_and_rejects_failed_or_override_dispatch();
#endif
  test_legacy_stale_epoch_and_no_mutation_paths();
  test_legacy_false_policy_disarms_without_changing_preference();
  test_legacy_explicit_restore_bypasses_policy_and_recent_apply_grace();
  test_recent_apply_uses_bounded_disconnect_settlement();
  test_explicit_restore_and_stop_cannot_be_cleared_by_owner_ping();
  test_repeated_disconnect_preserves_original_settlement_deadline();
  test_ping_does_not_clear_an_explicit_restore_request();
  test_ping_wire_format_keeps_plain_probes_unowned();
  std::cout << "display-helper recovery cases passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="display-helper-recovery-") as temporary:
    directory = pathlib.Path(temporary)
    source = directory / "test.cpp"
    source.write_text(program)
    if retained_source is not None:
        retained_source.parent.mkdir(parents=True, exist_ok=True)
        retained_source.write_text(program)
    binary = directory / "test"
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", f"-DHAS_SETTLEMENT={int(has_settlement)}", f"-DHAS_DISABLED_RECOVERY={int(has_disabled_recovery_parameter)}", f"-DHAS_RECOVERY_TICKET={int(has_recovery_ticket_parameter)}", f"-DHAS_PRESERVE={int(has_preserve)}", "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

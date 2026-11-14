"""Compile the production Sunshine recovery/transport functions with fake I/O.

The Windows API and driver boundary is replaced by a tiny in-memory transport.
The recovery orchestration, transport-health helper, transport replacement,
ping-thread startup, catalog preflight, provenance comparison, and reclaimed-display
policy are compiled from their production definitions.
"""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/platform/windows/virtual_display_sunshine.cpp").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("(", start)
    parens, closing = 0, opening
    while True:
        if text[closing] == "(":
            parens += 1
        elif text[closing] == ")":
            parens -= 1
            if parens == 0:
                break
        closing += 1
    brace = text.index("{", closing)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


production_attempt = extract_function(
    SOURCE,
    "bool attempt_virtual_display_recovery(RecoveryMonitorState &state, std::stop_token stop_token)",
)
ensure_declaration_start = SOURCE.index("  bool ensure_control_transport_responsive(")
ensure_declaration_end = SOURCE.index(";", ensure_declaration_start) + 1
production_ensure_declaration = SOURCE[ensure_declaration_start:ensure_declaration_end]
assert "{" not in production_ensure_declaration
assert ensure_declaration_start < SOURCE.index("bool attempt_virtual_display_recovery(")
production_ensure = extract_function(
    SOURCE,
    "bool ensure_control_transport_responsive(\n    std::string_view operation,\n    std::stop_token stop_token = {}",
)
production_control_snapshot = extract_function(
    SOURCE,
    "std::shared_ptr<sunshine_driver::WindowsControlTransport> control_transport_snapshot() {",
)
production_replace_transport = extract_function(
    SOURCE,
    "std::shared_ptr<sunshine_driver::WindowsControlTransport> replace_control_transport(",
)
production_clear_transport = extract_function(SOURCE, "void clear_control_transport()")
production_luid_equal = extract_function(SOURCE, "bool luid_equal(const LUID &lhs, const LUID &rhs)")
production_provenance_match = extract_function(
    SOURCE,
    "bool render_adapter_request_provenance_matches(\n      const uuid_util::uuid_t &guid,\n      const std::string_view context",
)
production_ping_start = extract_function(
    SOURCE,
    "static bool start_ping_thread_impl(\n    std::function<void()> failCb,",
)
production_catalog_preflight = "\n".join(
    extract_function(SOURCE, signature)
    for signature in (
        "std::shared_ptr<sunshine_driver::WindowsControlTransport> catalog_inspection_transport() {",
        "bool allowsSharedCatalogCapture() {",
        "bool supportsRequestedResolutionCatalog() {",
        "std::optional<bool> virtualDisplayCatalogMatches(const GUID &guid, uint32_t width, uint32_t height, bool requested_only) {",
    )
)


PROGRAM = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "src/platform/windows/display_helper_borrow_gate.h"
#include "src/platform/windows/virtual_display_policy.h"

#define BOOST_LOG(level) std::clog

// The extracted recovery functions run as a normal host process in this fake
// I/O fixture. Reuse the production local process gate; helper mode fails
// closed because this non-Windows harness does not provide its named mutex.
namespace display_helper::borrow {
  inline std::atomic_bool &helper_process() {
    static std::atomic_bool active {false};
    return active;
  }
  class native_mutation_scope_t {
  public:
    native_mutation_scope_t() = default;
    native_mutation_scope_t(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t &operator=(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t(native_mutation_scope_t &&) = default;
    explicit operator bool() const { return static_cast<bool>(local_); }
    static native_mutation_scope_t enter(bool wait, const std::function<bool()> &cancelled = {}) {
      auto local = wait ? process_gate().wait_enter(cancelled) : process_gate().try_enter();
      if (!local || helper_process().load(std::memory_order_acquire)) return {};
      return native_mutation_scope_t {std::move(local)};
    }
  private:
    explicit native_mutation_scope_t(gate_t::scope_t local): local_(std::move(local)) {}
    gate_t::scope_t local_;
  };
}

struct LUID { std::uint32_t LowPart = 0; std::int32_t HighPart = 0; };
using GUID = int;
namespace uuid_util {
  struct uuid_t {
    std::string value;
    std::string string() const { return value; }
  };
}
namespace VDISPLAY { struct ensure_display_result {}; }
namespace sunshine_driver {
  struct WindowsControlTransport {
    int id = 0;
    bool is_valid = true;
    bool is_responsive = true;
    int cancelled = 0;
    bool valid() const { return is_valid; }
    void cancel_pending_io() { ++cancelled; }
  };
}
namespace sunshine_driver {
  struct open_result {
    std::unique_ptr<WindowsControlTransport> transport;
    int status = 0;
    unsigned long native_error = 0;
    std::wstring device_path = L"producer";
    bool ok() const { return static_cast<bool>(transport); }
  };
  const char *to_string(int) { return "fake"; }
  struct device_info { std::wstring device_path; bool openable; };
  std::vector<device_info> enumerate_control_devices(std::uint32_t *error);
  open_result open_first_control_device();
  struct feed_result { bool ok() const { return true; } int status = 0; unsigned long native_error = 0; };
  struct LeaseRequest { std::uint64_t lease_id = 0; std::uint32_t requested_timeout_ms = 0; };
  struct ControlClient {
    WindowsControlTransport &transport;
    explicit ControlClient(WindowsControlTransport &value): transport(value) {}
    feed_result feed_lease(const LeaseRequest &) { return {}; }
  };
}

namespace VDISPLAY_SUNSHINE {
  using namespace std::chrono_literals;
  using Transport = sunshine_driver::WindowsControlTransport;

  enum class DRIVER_STATUS { UNKNOWN, OK, FAILED };
  enum class DRIVER_SELECTION { UNKNOWN, VIBESHINE };
  enum class OpenRecoveryBehavior { transport_only, recover_driver };
  enum class RestartCooldownBehavior { skip, wait };
  using ::VDISPLAY::ensure_display_result;
  namespace VDISPLAY {
    using DRIVER_STATUS = ::VDISPLAY_SUNSHINE::DRIVER_STATUS;
    using DRIVER_SELECTION = ::VDISPLAY_SUNSHINE::DRIVER_SELECTION;
    namespace policy = ::VDISPLAY::policy;
  }
  namespace proc {
    std::atomic<DRIVER_STATUS> status {DRIVER_STATUS::UNKNOWN};
    std::atomic<DRIVER_SELECTION> selection {DRIVER_SELECTION::UNKNOWN};
    void setVDisplayDriverStatus(DRIVER_STATUS value, DRIVER_SELECTION selected) {
      status.store(value);
      selection.store(selected);
    }
    std::atomic<DRIVER_STATUS> &vDisplayDriverStatus = status;
    std::atomic<DRIVER_SELECTION> &vDisplayDriverSelection = selection;
  }

  namespace util {
    template <typename F> struct fail_guard_t {
      F fn;
      ~fail_guard_t() { fn(); }
    };
    template <typename F> auto fail_guard(F fn) { return fail_guard_t<F> {std::move(fn)}; }
  }

  std::mutex g_control_transport_mutex;
  std::shared_ptr<Transport> VIRTUAL_DISPLAY_DRIVER_TRANSPORT;
  std::recursive_mutex g_watchdog_lifecycle_mutex;
  std::recursive_mutex g_virtual_display_operation_mutex;
  std::mutex g_watchdog_thread_mutex;
  std::mutex g_watchdog_wake_mutex;
  std::condition_variable g_watchdog_wake_cv;
  std::thread g_watchdog_thread;
  std::shared_ptr<Transport> g_watchdog_transport;
  std::function<void()> g_watchdog_fail_cb;
  std::atomic_bool g_watchdog_feed_requested {false};
  std::atomic_bool g_watchdog_stop_requested {false};
  std::atomic<std::int64_t> g_watchdog_grace_deadline_ns {0};
  std::uint64_t g_watchdog_wake_generation = 0;
  bool g_watchdog_start_in_progress = false;
  constexpr auto WATCHDOG_INIT_GRACE = 30s;
  constexpr std::uint32_t DRIVER_LEASE_TIMEOUT_MS = 30000;
  int next_transport_id = 1;
  int device_open_count = 0;
  bool open_succeeds = true;
  bool opened_transport_responsive = true;
  bool requested_resolution_catalog_supported = true;
  int requested_resolution_catalog_checks = 0;
  int retained_release_count = 0;
  int create_count = 0;
  bool last_requested_resolution_only = false;
  int catalog_queries = 0;
  int adapter_recoveries = 0;
  bool shared_catalog_allowed = true;
  int device_count = 1;
  bool lease_owned = false;

  namespace VDISPLAY::catalog_policy {
    enum class inspection { unknown, absent, matches, replace_owned };
    inspection retained_catalog = inspection::matches;
    bool supported(sunshine_driver::ControlClient &client) {
      ++catalog_queries;
      ++requested_resolution_catalog_checks;
      return client.transport.is_responsive && requested_resolution_catalog_supported;
    }
    bool allows_shared_capture(sunshine_driver::ControlClient &client) {
      ++catalog_queries;
      return client.transport.is_responsive && shared_catalog_allowed;
    }
    inspection inspect(sunshine_driver::ControlClient &client, std::uint64_t, std::uint32_t, std::uint32_t, bool) {
      ++catalog_queries;
      return client.transport.is_responsive ? retained_catalog : inspection::unknown;
    }
  }
  std::uint64_t client_uuid_to_virtual_display_id(const GUID &guid) { return guid; }

  struct LuidProvenance {
    std::weak_ptr<Transport> transport;
    LUID luid {};
  };
  std::mutex g_render_adapter_request_mutex;
  std::weak_ptr<Transport> g_current_render_adapter_request_transport;
  LUID g_current_render_adapter_request_luid {};
  std::unordered_map<std::string, LuidProvenance> g_render_adapter_request_provenance;

  std::shared_ptr<Transport> control_transport_snapshot();
  std::shared_ptr<Transport> replace_control_transport(std::unique_ptr<Transport> replacement);
  void clear_control_transport();

  bool driver_transport_responsive(Transport *transport) {
    return transport && transport->valid() && transport->is_responsive;
  }
  void stop_watchdog_thread() {
    std::thread thread;
    {
      std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
      g_watchdog_stop_requested.store(true);
      g_watchdog_wake_cv.notify_all();
      thread = std::move(g_watchdog_thread);
    }
    if (thread.joinable()) thread.join();
    std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
    g_watchdog_transport.reset();
  }
  void set_watchdog_feed_requested(bool value, bool) { g_watchdog_feed_requested.store(value); }
  bool check_driver_protocol_compatible(sunshine_driver::ControlClient &) { return true; }
  bool ensure_driver_is_ready_impl(RestartCooldownBehavior, std::stop_token) { return true; }
  void store_watchdog_fail_cb(const std::function<void()> &cb) { g_watchdog_fail_cb = cb; }
  std::function<void()> copy_watchdog_fail_cb() { return g_watchdog_fail_cb; }
  bool watchdog_thread_running() {
    std::lock_guard<std::mutex> lock(g_watchdog_thread_mutex);
    return g_watchdog_thread.joinable();
  }
  std::int64_t steady_ticks_from_time(std::chrono::steady_clock::time_point) { return 1; }
  bool within_grace_period(std::chrono::steady_clock::time_point) { return false; }
  bool wait_for_watchdog_state_change(std::chrono::milliseconds, std::uint64_t &) { return true; }
  void log_control_failure(std::string_view, int, unsigned long) {}
  void dispatch_watchdog_fail_cb(std::shared_ptr<const std::function<void()>>) {}

  struct Lease { std::uint64_t lease_id = 1; };
  struct LeaseTracker { std::vector<Lease> all() const { return lease_owned ? std::vector<Lease>{{}} : std::vector<Lease>{}; } };
  LeaseTracker &driver_lease_tracker() { static LeaseTracker tracker; return tracker; }

  DRIVER_STATUS open_vdisplay_device_with_status(std::stop_token, OpenRecoveryBehavior behavior) {
    if (behavior == OpenRecoveryBehavior::recover_driver) ++adapter_recoveries;
    if (!open_succeeds) {
      proc::setVDisplayDriverStatus(DRIVER_STATUS::FAILED, DRIVER_SELECTION::VIBESHINE);
      return DRIVER_STATUS::FAILED;
    }
    auto opened = sunshine_driver::open_first_control_device();
    auto transport = replace_control_transport(std::move(opened.transport));
    proc::setVDisplayDriverStatus(
      transport ? DRIVER_STATUS::OK : DRIVER_STATUS::FAILED,
      DRIVER_SELECTION::VIBESHINE
    );
    return transport ? DRIVER_STATUS::OK : DRIVER_STATUS::FAILED;
  }

  struct RecoveryParams {
    GUID guid = 17;
    std::string client_uid = "client";
    std::string client_name = "test-client";
    std::optional<std::string> hdr_profile;
    std::uint32_t width = 1920, height = 1080, fps = 60, base_fps_millihz = 60000;
    bool framegen_refresh_active = false;
    int framegen_refresh_multiplier = 1;
    std::optional<bool> hdr_requested;
    bool requested_resolution_only = false;
    std::function<std::function<void()>(const struct VirtualDisplayCreationResult &, std::stop_token)> on_recovery_success;
  };
  struct RecoveryMonitorState {
    RecoveryParams params;
    bool abort = false;
    int identifier_updates = 0;
    std::string describe_target() const { return "test target"; }
    void update_identifiers(const std::optional<std::wstring> &, const std::optional<std::string> &, const std::optional<std::wstring> &) { ++identifier_updates; }
  };
  bool supportsRequestedResolutionCatalog();
  bool release_retained_ensure_display_for_stream(GUID, const char *, std::stop_token token) {
    ++retained_release_count;
    return !token.stop_requested();
  }
  bool lock_recovery_operation(std::unique_lock<std::recursive_mutex> &lock, const RecoveryMonitorState &state, std::stop_token token) {
    return !state.abort && !token.stop_requested() && lock.try_lock();
  }
  bool monitor_should_abort(const RecoveryMonitorState &state, std::stop_token token) { return state.abort || token.stop_requested(); }
  void setWatchdogFeedingEnabled(bool) {}
  bool remove_virtual_display_impl(GUID, bool, std::stop_token) { return true; }

  struct VirtualDisplayCreationResult {
    std::optional<std::wstring> display_name {L"\\\\.\\DISPLAY9"};
    std::optional<std::string> device_id {"device-9"};
    std::optional<std::wstring> monitor_device_path {L"monitor-path-9"};
  };
  bool provenance_matches = false;
  bool display_recreated = false;
  bool render_adapter_request_provenance_matches(const uuid_util::uuid_t &, std::string_view);
  std::optional<VirtualDisplayCreationResult> create_virtual_display_with_stop(
    const char *, const char *, const char *, std::uint32_t, std::uint32_t, std::uint32_t,
    GUID guid, std::uint32_t, bool, int, std::optional<bool>, bool, bool, bool,
    bool requested_resolution_only, std::stop_token
  ) {
    ++create_count;
    last_requested_resolution_only = requested_resolution_only;
    provenance_matches = render_adapter_request_provenance_matches(uuid_util::uuid_t {std::to_string(guid)}, "test reclaim");
    const auto plan = VDISPLAY::policy::reclaimed_display_plan_for_session(
      true, true, true, provenance_matches
    );
    display_recreated = plan.action == VDISPLAY::policy::reclaimed_display_action::recreate;
    return VirtualDisplayCreationResult {};
  }

''' + production_control_snapshot + production_replace_transport + production_clear_transport + r'''
  void closeVDisplayDevice() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(g_watchdog_lifecycle_mutex);
    stop_watchdog_thread();
    set_watchdog_feed_requested(false, false);
    g_watchdog_grace_deadline_ns.store(0);
    clear_control_transport();
  }
''' + production_ensure_declaration + r'''
  static bool start_ping_thread_impl(std::function<void()>, std::stop_token, OpenRecoveryBehavior);
''' + production_attempt + production_ensure + production_catalog_preflight + production_luid_equal + production_provenance_match + production_ping_start + r'''

  void reset_fixture() {
    stop_watchdog_thread();
    clear_control_transport();
    // The successful recovery case installs a callback that can restart the
    // production ping thread. Do not leak it into later independent cases.
    g_watchdog_fail_cb = {};
    {
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      g_current_render_adapter_request_transport.reset();
      g_current_render_adapter_request_luid = {};
      g_render_adapter_request_provenance.clear();
    }
    device_open_count = 0;
    next_transport_id = 1;
    open_succeeds = true;
    opened_transport_responsive = true;
    requested_resolution_catalog_supported = true;
    requested_resolution_catalog_checks = 0;
    retained_release_count = 0;
    create_count = 0;
    last_requested_resolution_only = false;
    catalog_queries = 0;
    adapter_recoveries = 0;
    shared_catalog_allowed = true;
    device_count = 1;
    lease_owned = false;
    VDISPLAY::catalog_policy::retained_catalog = VDISPLAY::catalog_policy::inspection::matches;
    provenance_matches = false;
    display_recreated = false;
    proc::setVDisplayDriverStatus(DRIVER_STATUS::UNKNOWN, DRIVER_SELECTION::UNKNOWN);
  }
  void seed_provenance(const uuid_util::uuid_t &guid, const std::shared_ptr<Transport> &transport, LUID luid) {
    std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
    g_current_render_adapter_request_transport = transport;
    g_current_render_adapter_request_luid = luid;
    g_render_adapter_request_provenance[guid.string()] = {transport, luid};
  }
  int failures = 0;
  void check(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    else std::cout << "PASS: " << message << '\n';
  }

  void test_healthy_recovery_preserves_transport_provenance() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    const uuid_util::uuid_t guid {"17"};
    const LUID luid {7, 0};
    seed_provenance(guid, original, luid);
    g_watchdog_fail_cb = [] {};
    RecoveryMonitorState state;
    const bool recovered = attempt_virtual_display_recovery(state, {});
    check(recovered, "healthy transport recovery completes");
    check(proc::vDisplayDriverStatus.load() == DRIVER_STATUS::OK &&
            proc::vDisplayDriverSelection.load() == DRIVER_SELECTION::VIBESHINE,
          "successful recovery restores driver status OK and selection VIBESHINE from UNKNOWN");
    check(control_transport_snapshot() == original, "healthy recovery preserves shared transport identity");
    check(provenance_matches, "same transport and LUID provenance remains valid after reclaim");
    check(!display_recreated, "reclaimed inactive display with matching provenance reuses in place");
    check(g_watchdog_transport && g_watchdog_transport != original,
          "watchdog ping uses a separately opened handle");
  }

  void test_ping_open_does_not_replace_shared_transport() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    const bool started = start_ping_thread_impl([] {}, {}, OpenRecoveryBehavior::transport_only);
    check(started, "watchdog ping starts with a healthy shared transport");
    check(g_watchdog_transport && g_watchdog_transport != original,
          "watchdog ping opens its own handle");
    check(control_transport_snapshot() == original,
          "opening the independent ping handle leaves shared transport unchanged");
  }

  void test_unhealthy_recovery_reopens_and_recreates_for_stale_provenance() {
    reset_fixture();
    auto original = std::make_shared<Transport>(Transport {1, true, false, 0});
    VIRTUAL_DISPLAY_DRIVER_TRANSPORT = original;
    const uuid_util::uuid_t guid {"17"};
    seed_provenance(guid, original, LUID {7, 0});
    g_watchdog_fail_cb = [] {};
    RecoveryMonitorState state;
    const bool recovered = attempt_virtual_display_recovery(state, {});
    check(recovered, "recovery succeeds after reopening an unhealthy shared transport");
    check(control_transport_snapshot() != original, "unhealthy recovery replaces the shared transport");
    check(original->cancelled == 1, "reopen cancels pending I/O on the unhealthy transport");
    check(!provenance_matches, "reopened transport does not inherit old creation provenance");
    check(display_recreated, "unhealthy reopen keeps fail-closed recreate policy for old provenance");
  }

  void test_same_transport_with_changed_luid_still_recreates() {
    reset_fixture();
    auto transport = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    const uuid_util::uuid_t guid {"17"};
    seed_provenance(guid, transport, LUID {7, 0});
    {
      std::lock_guard<std::mutex> lock(g_render_adapter_request_mutex);
      g_current_render_adapter_request_luid = LUID {8, 0};
    }
    const bool matches = render_adapter_request_provenance_matches(guid, "changed LUID");
    const auto plan = VDISPLAY::policy::reclaimed_display_plan_for_session(true, true, true, matches);
    check(!matches, "same transport with a different current LUID fails provenance comparison");
    check(plan.action == VDISPLAY::policy::reclaimed_display_action::recreate,
          "different LUID still forces recreation");
  }

  void test_health_gate_preserves_healthy_transport() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    const bool ready = ensure_control_transport_responsive("fixture", {}, OpenRecoveryBehavior::transport_only);
    check(ready, "health gate accepts responsive transport");
    check(control_transport_snapshot() == original, "health gate avoids reopening responsive transport");
    check(device_open_count == 0, "health gate opens no new control device when transport is responsive");
  }

  void test_unhealthy_transport_reopens_and_fails_closed() {
    reset_fixture();
    auto old_transport = std::make_shared<Transport>(Transport {1, true, false, 0});
    VIRTUAL_DISPLAY_DRIVER_TRANSPORT = old_transport;
    open_succeeds = false;
    const bool ready = ensure_control_transport_responsive("fixture", {}, OpenRecoveryBehavior::transport_only);
    check(!ready, "unresponsive transport plus failed reopen fails closed");
    check(!control_transport_snapshot(), "failed reopen leaves no cached transport");
    check(old_transport->cancelled == 1, "failed recovery cancels pending I/O on the old transport");
  }

  void test_opened_but_unresponsive_transport_is_rejected() {
    reset_fixture();
    VIRTUAL_DISPLAY_DRIVER_TRANSPORT = std::make_shared<Transport>(Transport {1, true, false, 0});
    opened_transport_responsive = false;
    const bool ready = ensure_control_transport_responsive("fixture", {}, OpenRecoveryBehavior::transport_only);
    check(!ready, "new transport that fails its health check is rejected");
    check(!control_transport_snapshot(), "unresponsive new transport is cleared");
  }

  void test_cancelled_recovery_does_not_open_transport() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    RecoveryMonitorState state;
    std::stop_source source;
    source.request_stop();
    const bool recovered = attempt_virtual_display_recovery(state, source.get_token());
    check(!recovered, "cancelled recovery exits before opening the driver");
    check(control_transport_snapshot() == original, "cancellation preserves the existing shared transport");
    check(device_open_count == 0, "cancelled recovery performs no device open");
  }

  void test_requested_resolution_only_recovery_fails_before_retained_probe_without_catalog() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    requested_resolution_catalog_supported = false;
    RecoveryMonitorState state;
    state.params.requested_resolution_only = true;
    const bool recovered = attempt_virtual_display_recovery(state, {});
    check(!recovered, "requested-resolution-only recovery fails when the producer lacks catalog support");
    check(requested_resolution_catalog_checks == 1, "requested-resolution-only recovery checks catalog support");
    check(retained_release_count == 0, "unsupported requested-resolution-only recovery keeps retained display probe");
    check(create_count == 0, "unsupported requested-resolution-only recovery does not recreate a display");
    check(control_transport_snapshot() == original, "catalog capability failure preserves the healthy transport");
  }

  void test_requested_resolution_only_recovery_propagates_catalog_policy() {
    reset_fixture();
    replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    RecoveryMonitorState state;
    state.params.requested_resolution_only = true;
    const bool recovered = attempt_virtual_display_recovery(state, {});
    check(recovered, "requested-resolution-only recovery completes with catalog support");
    check(requested_resolution_catalog_checks == 1, "supported requested-resolution-only recovery checks catalog once");
    check(retained_release_count == 1, "supported requested-resolution-only recovery releases the retained probe");
    check(create_count == 1, "supported requested-resolution-only recovery recreates once");
    check(last_requested_resolution_only, "requested-resolution-only policy reaches production create boundary");
  }

  void test_catalog_preflight_opens_transport_on_first_launch() {
    // Startup probing can use a physical display and never open the VDD.
    // Exercise all three production entry points with that initial state.
    for (int query = 0; query < 3; ++query) {
      reset_fixture();
      if (query == 0) check(allowsSharedCatalogCapture(), "cold shared-capture preflight queries the producer");
      if (query == 1) check(supportsRequestedResolutionCatalog(), "cold capability preflight queries the producer");
      if (query == 2) check(virtualDisplayCatalogMatches(17, 1920, 1080, false) == true,
                            "cold retained-catalog preflight queries the producer");
      check(device_open_count == 1, "catalog preflight opens a missing transport once");
      check(catalog_queries == 1, "catalog preflight reaches producer policy after opening");
      check(adapter_recoveries == 0, "catalog inspection never requests adapter recovery");
      check(create_count == 0 && retained_release_count == 0, "catalog inspection preserves existing displays");
    }
  }

  void test_catalog_preflight_preserves_healthy_transport_and_denials() {
    reset_fixture();
    auto original = replace_control_transport(std::make_unique<Transport>(Transport {1, true, true, 0}));
    lease_owned = true;
    seed_provenance(uuid_util::uuid_t {"17"}, original, LUID {7, 0});
    shared_catalog_allowed = false;
    requested_resolution_catalog_supported = false;
    VDISPLAY::catalog_policy::retained_catalog = VDISPLAY::catalog_policy::inspection::unknown;
    check(!allowsSharedCatalogCapture(), "private catalog still rejects a shared join");
    check(!supportsRequestedResolutionCatalog(), "unsupported producer still rejects private catalog requests");
    check(!virtualDisplayCatalogMatches(17, 1920, 1080, false).has_value(), "unknown producer catalog still rejects reuse");
    check(control_transport_snapshot() == original && device_open_count == 0,
          "catalog preflight retains the healthy transport");
    check(render_adapter_request_provenance_matches(uuid_util::uuid_t {"17"}, "catalog fixture"),
          "catalog preflight retains adapter provenance");
    VDISPLAY::catalog_policy::retained_catalog = VDISPLAY::catalog_policy::inspection::absent;
    check(virtualDisplayCatalogMatches(17, 1920, 1080, false) == false,
          "absent display permits creation instead of becoming an unknown catalog");
  }

  void test_catalog_preflight_preserves_stale_transport_and_fails_closed() {
    for (int query = 0; query < 3; ++query) {
      reset_fixture();
      auto stale = replace_control_transport(std::make_unique<Transport>(Transport {1, true, false, 0}));
      lease_owned = true;
      if (query == 0) check(!allowsSharedCatalogCapture(), "stale producer rejects shared capture");
      if (query == 1) check(!supportsRequestedResolutionCatalog(), "stale producer rejects capability query");
      if (query == 2) check(!virtualDisplayCatalogMatches(17, 1920, 1080, false), "stale producer rejects catalog query");
      check(control_transport_snapshot() == stale && stale->cancelled == 0 && device_open_count == 0,
            "inspection preserves the shared producer");
    }
    reset_fixture();
    auto stale_unowned = replace_control_transport(std::make_unique<Transport>(Transport {1, true, false, 0}));
    check(supportsRequestedResolutionCatalog(), "stale unowned handle permits an independent inspection");
    check(control_transport_snapshot() == stale_unowned && stale_unowned->cancelled == 0, "independent inspection preserves stale shared handle");
    reset_fixture();
    device_count = 2;
    check(!supportsRequestedResolutionCatalog() && device_open_count == 0, "ambiguous producers fail closed");
    reset_fixture();
    lease_owned = true;
    check(!allowsSharedCatalogCapture() && device_open_count == 0, "unbound retained lease fails closed");
    reset_fixture();
    open_succeeds = false;
    check(!supportsRequestedResolutionCatalog(), "failed inspection open fails closed");
  }

  int run() {
    [[maybe_unused]] auto cleanup_fixture = util::fail_guard([] {
      stop_watchdog_thread();
      clear_control_transport();
      g_watchdog_fail_cb = {};
    });
    test_healthy_recovery_preserves_transport_provenance();
    test_ping_open_does_not_replace_shared_transport();
    test_unhealthy_recovery_reopens_and_recreates_for_stale_provenance();
    test_same_transport_with_changed_luid_still_recreates();
    test_health_gate_preserves_healthy_transport();
    test_unhealthy_transport_reopens_and_fails_closed();
    test_opened_but_unresponsive_transport_is_rejected();
    test_cancelled_recovery_does_not_open_transport();
    test_requested_resolution_only_recovery_fails_before_retained_probe_without_catalog();
    test_requested_resolution_only_recovery_propagates_catalog_policy();
    test_catalog_preflight_opens_transport_on_first_launch();
    test_catalog_preflight_preserves_healthy_transport_and_denials();
    test_catalog_preflight_preserves_stale_transport_and_fails_closed();
    std::cout << "Recovery transport fixture failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
  }
}

namespace sunshine_driver {
  std::vector<device_info> enumerate_control_devices(std::uint32_t *error) {
    *error = 0;
    return std::vector<device_info>(VDISPLAY_SUNSHINE::device_count, {L"producer", true});
  }
  open_result open_first_control_device() {
    using namespace VDISPLAY_SUNSHINE;
    ++device_open_count;
    if (!open_succeeds) return {};
    auto transport = std::make_unique<WindowsControlTransport>();
    transport->id = next_transport_id++;
    transport->is_responsive = opened_transport_responsive;
    return {std::move(transport)};
  }
}

int main() { return VDISPLAY_SUNSHINE::run(); }
'''


with tempfile.TemporaryDirectory(prefix="virtual-display-recovery-") as temporary:
    directory = pathlib.Path(temporary)
    cpp = directory / "test.cpp"
    binary = directory / "test-recovery"
    cpp.write_text(PROGRAM)
    subprocess.run(
        [
            "g++", "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-Wno-missing-field-initializers", "-I", str(ROOT), str(cpp), "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=10)

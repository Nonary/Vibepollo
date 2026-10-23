"""Execute production restore admission and client handshake paths with OS fakes.

The fixtures compile real function bodies, replacing only the monotonic clock and
Windows boundary collaborators. --repo permits before/after failure controls.
This verifies application timeout propagation, not Windows driver scheduling or
physical TV power transitions.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--repo', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
parser.add_argument('--compiler', default='g++')
parser.add_argument('--fixture', choices=['admission', 'preflight', 'transport', 'resume', 'target-entry', 'all'], default='all')
parser.add_argument('--integration-source', type=pathlib.Path)
parser.add_argument('--nvhttp-source', type=pathlib.Path)
parser.add_argument('--pipes-source', type=pathlib.Path)
parser.add_argument('--client-source', type=pathlib.Path)
args = parser.parse_args()
root = args.repo.resolve()


def function(source, signature):
    start = source.index(signature)
    while True:
        opening = re.search(r'\)\s*(?:const\s*)?\{', source[start:])
        brace = start + opening.end() - 1
        if ';' not in source[start:brace]:
            break
        start = source.index(signature, start + len(signature))
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace('std::chrono::steady_clock::now()', 'clock_now()')


common = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using Time = std::chrono::steady_clock::time_point;
Time fake_now = Time{} + 100s;
Time clock_now() { return fake_now; }
void advance(int ms) { fake_now += std::chrono::milliseconds(ms); }
std::ostringstream logs;
#define BOOST_LOG(level) logs
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n' << logs.str(); std::abort(); }
}
'''


def statement(source, signature, start=0):
    begin = source.index(signature, start)
    opening = source.index('(', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '(') - (source[end] == ')')
        end += 1
    return source[begin:source.index(';', end) + 1]


def admission(preflight=False):
    integration = (args.integration_source or root / 'src/platform/windows/display_helper_integration.cpp').read_text()
    nvhttp = (args.nvhttp_source or root / 'src/nvhttp.cpp').read_text()
    start = nvhttp.index('const bool virtual_display_mutation_allowed =')
    end = nvhttp.index('        if (!virtual_display_mutation_allowed)', start)
    admission_block = nvhttp[start:end].replace('std::chrono::steady_clock::now()', 'clock_now()')
    mutation_gate_start = nvhttp.rfind('if (request_virtual_display) {', 0, start)
    mutation_gate = function(nvhttp[mutation_gate_start:], 'if (request_virtual_display)')
    mutation_call = statement(nvhttp, 'apply_virtual_display_request(', end)
    startup_predicates = re.findall(
        r'const auto display_startup_cancelled = \[display_startup_deadline\] \{.*?\n[ \t]+\};', nvhttp, re.S)
    assert len(startup_predicates) == 2, 'expected launch and resume startup predicates'
    startup_predicate_functions = '\n'.join(
        f'std::function<bool()> production_startup_cancelled_{i}(Time display_startup_deadline) {{\n' +
        body.replace('std::chrono::steady_clock::now()', 'clock_now()') +
        '\nreturn display_startup_cancelled;\n}' for i, body in enumerate(startup_predicates))
    tv_block = function(nvhttp, 'if (!request_virtual_display && !preserve_requested_output && VDISPLAY::should_auto_enable_virtual_display())')
    functions = '\n'.join(function(integration, signature) for signature in [
        'bool restore_expected_with_live_helper(', 'bool disarm_helper_restore_if_running(',
        'static void invalidate_apply_verification(',
    ])
    public_functions = '\n'.join(function(integration, signature) for signature in [
        'bool disarm_pending_restore(', 'bool restore_in_progress(',
    ])
    factory_signature = 'std::function<void()> make_stream_start_restore_disarm('
    if factory_signature in nvhttp:
        disarm_factory = function(nvhttp, factory_signature)
    else:
        # Allow the new ordered-caller regression to execute against frozen v2.
        # Its real preflight/admission statements below still come from v2.
        disarm_factory = '''std::function<void()> make_stream_start_restore_disarm(
          std::function<bool()> cancelled, Time deadline) {
          return [=] { (void) display_helper_integration::disarm_pending_restore(cancelled, deadline, true); };
        }'''
    ordered_wrappers = ''
    for index in range(2):
        # New comments differ, so find the actual .before boundary relative to
        # the corresponding production cancellation predicate instead.
        predicate_start = nvhttp.index(startup_predicates[index])
        before = statement(nvhttp, 'request_audio_hold.before(', predicate_start)
        apply_start = nvhttp.index('auto request = display_helper_integration::helpers::build_request_from_session', predicate_start)
        preapply_start = nvhttp.rfind('disarm_startup_restore();', predicate_start, apply_start)
        if preapply_start >= 0:
            preapply = statement(nvhttp, 'disarm_startup_restore(', preapply_start)
        else:
            preapply_start = nvhttp.rfind('(void) display_helper_integration::disarm_pending_restore(', predicate_start, apply_start)
            preapply = statement(nvhttp, '(void) display_helper_integration::disarm_pending_restore(', preapply_start)
        for name, body in [('preflight', before), ('preapply', preapply)]:
            ordered_wrappers += f'''void production_{name}_{index}(
              [[maybe_unused]] const std::function<void()> &disarm_startup_restore,
              [[maybe_unused]] Time display_startup_deadline,
              [[maybe_unused]] std::function<bool()> display_startup_cancelled) {{
              [[maybe_unused]] audio::display_change_hold_t request_audio_hold {{[] {{ return std::shared_ptr<void>{{}}; }}}};
              {body}
            }}\n'''
    ordered_tests = r'''
  // Execute the real synchronous .before statement, then production admission,
  // then the real pre-APPLY statement, sharing the request's one operation.
  for (int route : {0, 1}) {
    for (bool recent : {true, false}) {
      for (int failure : {1, 0, 2, 3}) {
        reset();
        if (recent) g_last_revert_us = now_steady_us();
        if (failure == 1) { reset_ok = false; second_send_ok = false; }
        if (failure == 2) ping_ok = false;
        if (failure == 3) second_send_ok = false;
        const auto deadline = fake_now + 2s;
        auto cancelled = route == 0 ? production_startup_cancelled_0(deadline) : production_startup_cancelled_1(deadline);
        const auto disarm = make_stream_start_restore_disarm(cancelled, deadline);
        const auto preflight = route == 0 ? production_preflight_0 : production_preflight_1;
        const auto preapply = route == 0 ? production_preapply_0 : production_preapply_1;
        preflight(disarm, deadline, cancelled);
        std::cout << "production_preflight: route=" << route << " recent=" << recent << " failure=" << failure
                  << " guard=" << g_restore_expected << " resets=" << reset_calls << " killed=" << killed << std::endl;
        require(killed == 0, "production preflight terminated a live restoring helper before admission");
        require(reset_calls == 1, "production preflight bypassed bounded stale-pipe recovery");
        const auto elapsed_after_preflight = fake_now;
        int mutations = 0;
        production_mutation(deadline, cancelled, disarm, mutations);
        // Enumeration/preparation can exceed the legacy DISARM throttle.
        advance(200);
        preapply(disarm, deadline, cancelled);
        std::cout << "ordered_start: route=" << route << " recent=" << recent << " failure=" << failure
                  << " mutation=" << mutations << " guard=" << g_restore_expected
                  << " resets=" << reset_calls << " killed=" << killed << std::endl;
        require(mutations == (failure == 0 ? 1 : 0), "ordered startup admitted failed recovery or rejected recovered state");
        require(g_restore_expected == (failure != 0) && killed == 0, "ordered startup lost restore guard after failed recovery");
        require(reset_calls == 1 && fake_now == elapsed_after_preflight + 200ms,
                "admission/pre-APPLY renewed the request's recovery budget");
      }
    }
    for (auto event : {Interruption::Shutdown, Interruption::Cancel, Interruption::Deadline}) {
      reset(); g_last_revert_us = now_steady_us(); reset_ok = false;
      interrupt_on_probe = 1; interrupted_probe_live = false; interruption = event;
      const auto deadline = fake_now + 2s;
      auto cancelled = [=] { return cancelled_by_probe || production_startup_cancelled_0(deadline)(); };
      const auto disarm = make_stream_start_restore_disarm(cancelled, deadline);
      (route == 0 ? production_preflight_0 : production_preflight_1)(disarm, deadline, cancelled);
      require(!admit(deadline, cancelled, disarm) && g_restore_expected && killed == 0 && reset_calls == 0,
              "interrupted ordered preflight admitted mutation or lost the known guard");
    }
    reset(); reset_delay = 800; g_last_revert_us = now_steady_us();
    const auto start = fake_now, deadline = fake_now + 2s;
    auto cancelled = production_startup_cancelled_0(deadline);
    const auto disarm = make_stream_start_restore_disarm(cancelled, deadline);
    (route == 0 ? production_preflight_0 : production_preflight_1)(disarm, deadline, cancelled);
    require(!admit(deadline, cancelled, disarm) && g_restore_expected && reset_calls == 1 && killed == 0,
            "expired ordered recovery retried or lost the guard");
    require(fake_now <= start + 1ms + 750ms, "ordered startup extended recovery budget");
    reset(); const auto short_deadline = fake_now + 15ms;
    auto short_cancelled = production_startup_cancelled_0(short_deadline);
    const auto short_disarm = make_stream_start_restore_disarm(short_cancelled, short_deadline);
    (route == 0 ? production_preflight_0 : production_preflight_1)(short_disarm, short_deadline, short_cancelled);
    require(!admit(short_deadline, short_cancelled, short_disarm) && g_restore_expected && fake_now <= short_deadline,
            "ordered startup extended overall deadline");
    // A later REVERT may not be cancelled by a second cached/legacy attempt.
    reset(); first_send_ok = true;
    const auto rearm_deadline = fake_now + 2s;
    auto rearm_cancelled = production_startup_cancelled_0(rearm_deadline);
    const auto rearm_disarm = make_stream_start_restore_disarm(rearm_cancelled, rearm_deadline);
    (route == 0 ? production_preflight_0 : production_preflight_1)(rearm_disarm, rearm_deadline, rearm_cancelled);
    g_restore_expected = true; ++g_restore_generation; g_last_revert_us = now_steady_us();
    require(!admit(rearm_deadline, rearm_cancelled, rearm_disarm) && g_restore_expected && fast_calls == 1,
            "later restore generation was silently superseded by a reused startup operation");
  }
  std::cout << "16 ordered launch/resume recovery cases plus 12 interruption/budget/generation cases passed\n";
'''
    return common + r'''
std::atomic<bool> g_restore_expected {true};
std::atomic<long long> g_last_revert_us {0}, g_last_disarm_attempt_us {0}, g_last_disarm_success_us {0};
std::atomic<unsigned long long> g_restore_generation {1}, g_disarm_generation_sent {0}, g_last_apply_generation {0};
constexpr auto kDisarmRestoreBudget = 150ms;
constexpr auto kDisarmRetryThrottle = 150ms;
constexpr auto kStreamStartDisarmReconnectBudget = 750ms;
bool shutting_down = false, helper_live = true, cancelled_by_probe = false;
int helper_probe_calls = 0, interrupt_on_probe = 0;
bool interrupted_probe_live = false;
enum class Interruption { Shutdown, Cancel, Deadline };
Interruption interruption = Interruption::Shutdown;
int fast_calls = 0, reset_calls = 0, ping_calls = 0, killed = 0;
int fast_delay = 1, reset_delay = 10, ping_delay = 10, probe_delay = 0;
bool first_send_ok = false, second_send_ok = true, reset_ok = true, ping_ok = true;
bool accept_late_send = false;
Time reconnect_limit {};
long long now_steady_us() { return std::chrono::duration_cast<std::chrono::microseconds>(fake_now.time_since_epoch()).count(); }
bool shutdown_requested() { return shutting_down; }
bool cancellation_requested(const std::function<bool()> &cancelled) { return cancelled && cancelled(); }
bool operation_deadline_expired(Time deadline) { return fake_now >= deadline; }
bool helper_process_running(const std::function<bool()> &, Time deadline) {
  advance(probe_delay);
  if (++helper_probe_calls == interrupt_on_probe) {
    helper_live = interrupted_probe_live;
    switch (interruption) {
      case Interruption::Shutdown: shutting_down = true; break;
      case Interruption::Cancel: cancelled_by_probe = true; break;
      case Interruption::Deadline: fake_now = deadline; break;
    }
  }
  return helper_live;
}
namespace mail {
  constexpr int shutdown = 1;
  struct Event { bool peek() const { return shutting_down; } };
  struct Manager {
    template<class T> std::shared_ptr<Event> event(int) { return std::make_shared<Event>(); }
  } manager;
  Manager *man = &manager;
}
std::mutex &helper_mutex() { static std::mutex m; return m; }
struct Process { void terminate() { ++killed; } } process;
Process &helper_proc() { return process; }
bool sleep_with_cancellation(std::chrono::milliseconds wait, const std::function<bool()> &c, Time d) {
  fake_now += wait; return !cancellation_requested(c) && fake_now < d;
}
std::mutex &pending_apply_execution_mutex() { static std::mutex m; return m; }
bool lock_pending_apply_execution(std::unique_lock<std::mutex> &lock, const std::function<bool()> &c, Time d) {
  if (cancellation_requested(c) || operation_deadline_expired(d)) return false;
  lock.lock(); return true;
}
namespace platf::display_helper_client {
  bool send_disarm_restore_fast(int timeout, Time deadline) {
    ++fast_calls;
    const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - fake_now).count());
    advance(std::max(0, std::min({timeout, fast_delay, remaining})));
    return (fast_calls == 1 ? first_send_ok : second_send_ok) && (accept_late_send || fake_now < deadline);
  }
  bool reset_connection_cancellable(const std::function<bool()> &cancelled, Time deadline) {
    ++reset_calls; reconnect_limit = deadline;
    advance(std::max(0, std::min(reset_delay, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - fake_now).count()))));
    return reset_ok && !cancelled() && fake_now < deadline;
  }
  bool send_ping_cancellable(int, const std::function<bool()> &cancelled, Time deadline) {
    ++ping_calls; require(deadline == reconnect_limit, "PING got a renewed deadline");
    advance(std::max(0, std::min(ping_delay, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - fake_now).count()))));
    return ping_ok && !cancelled() && fake_now < deadline;
  }
}
''' + startup_predicate_functions + '\n' + functions + r'''
namespace display_helper_integration {
  bool disarm_pending_restore(std::function<bool()> = {}, Time = Time::max(), bool = false);
  bool restore_in_progress(std::function<bool()> = {}, Time = Time::max());
''' + public_functions + r'''
}
''' + disarm_factory + '\n' + (root / 'src/audio_display_transition.h').read_text().replace('#pragma once', '') + '\n' + ordered_wrappers + '\n' + function((root / 'src/platform/windows/display_helper_request_policy.cpp').read_text(), 'bool virtual_display_mutation_allowed(') + '\n' + function((root / 'src/platform/windows/display_helper_request_policy.cpp').read_text(), 'bool supersede_restore_for_virtual_display(') + r'''
namespace display_helper_integration::request_policy {
  using ::supersede_restore_for_virtual_display;
}
namespace VDISPLAY { bool monitor_off = true; bool should_auto_enable_virtual_display() { return monitor_off; } }
namespace config { void set_runtime_output_name_override(std::optional<std::string>) {} }
bool admit(Time display_startup_deadline, std::function<bool()> display_startup_cancelled,
           [[maybe_unused]] const std::function<void()> &disarm_startup_restore) {
''' + admission_block + r'''
  return virtual_display_mutation_allowed;
}
bool admit(Time deadline, std::function<bool()> cancelled) {
  const auto disarm = make_stream_start_restore_disarm(cancelled, deadline);
  return admit(deadline, cancelled, disarm);
}
void production_mutation(Time display_startup_deadline, std::function<bool()> display_startup_cancelled,
                         [[maybe_unused]] const std::function<void()> &disarm_startup_restore, int &mutations) {
  struct Session {
    bool virtual_display = true, virtual_display_failed = false;
    std::array<uint8_t, 16> virtual_display_guid_bytes {};
    std::string virtual_display_device_id;
    std::optional<Time> virtual_display_ready_since;
    std::optional<bool> virtual_display_hdr_enabled;
  } session, *launch_session = &session;
  const bool request_virtual_display = true;
  auto apply_framegen_refresh_policy = [](bool) {};
  auto apply_virtual_display_request = [&](bool requested) { if (requested) ++mutations; };
''' + mutation_gate + '\n' + mutation_call + r'''
}
bool first_launch_tv_off(Time deadline, bool include_identity_log_fields) {
  bool request_virtual_display = false, preserve_requested_output = false;
  [[maybe_unused]] bool has_app_output_override = false, pinned_virtual_display_mode_disabled = false;
  [[maybe_unused]] std::optional<std::string> app_output_override, pending_output_override;
  struct TVSession { std::optional<std::string> output_name_override, virtual_display_mode_override; };
  [[maybe_unused]] TVSession session, *launch_session = &session;
''' + tv_block + r'''
  return request_virtual_display && admit(deadline, [=] { return fake_now >= deadline; });
}
void reset() {
  fake_now = Time{} + 100s; g_restore_expected = true; g_restore_generation = 1;
  g_last_revert_us = 0; g_last_disarm_attempt_us = 0; g_last_disarm_success_us = 0;
  g_disarm_generation_sent = 0; g_last_apply_generation = 0; fast_calls = reset_calls = ping_calls = killed = 0;
  fast_delay = 1; reset_delay = ping_delay = 10; probe_delay = 0;
  first_send_ok = false; second_send_ok = reset_ok = ping_ok = helper_live = true;
  shutting_down = false; accept_late_send = false; cancelled_by_probe = false;
  helper_probe_calls = interrupt_on_probe = 0; interrupted_probe_live = false;
}
int main() {
''' + (ordered_tests if preflight else '') + r'''
  for (bool include_identity_log_fields : {true, false}) {
    reset();
    require(first_launch_tv_off(fake_now + 2s, include_identity_log_fields), "TV-off first launch did not recover its stale pipe");
    require(fast_calls == 2 && reset_calls == 1 && ping_calls == 1 && !g_restore_expected && killed == 0,
            "successful reconnect did not dispatch exactly one retry and clear restore guard");
  }
  reset(); ping_ok = false; g_last_revert_us = now_steady_us();
  require(!admit(fake_now + 2s, [] { return false; }) && g_restore_expected && killed == 0,
          "failed admission erased guard or killed a restoring helper");
  reset(); reset_ok = false;
  require(!admit(fake_now + 2s, [] { return false; }) && g_restore_expected && ping_calls == 0,
          "reset failure admitted mutation or continued to PING");
  require(g_last_apply_generation == 1, "failed reconnect retained stale capture/cleanup proof");
  reset(); second_send_ok = false;
  require(!admit(fake_now + 2s, [] { return false; }) && g_restore_expected && fast_calls == 2,
          "retry send failure erased the restore guard");
  reset(); const auto reconnect_start = fake_now; reset_delay = 800;
  require(!admit(fake_now + 2s, [] { return false; }) && g_restore_expected && ping_calls == 0,
          "expired reconnect continued or admitted mutation");
  require(fake_now <= reconnect_start + 1ms + 750ms, "reset restarted the 750ms reconnect budget");
  reset(); reset_delay = 740; ping_delay = 5; fast_delay = 10; accept_late_send = true;
  require(!admit(fake_now + 2s, [] { return false; }) && g_restore_expected,
          "DISARM success delivered after reconnect expiry erased the guard");
  reset(); const auto deadline = fake_now + 15ms;
  require(!admit(deadline, [=] { return fake_now >= deadline; }) && g_restore_expected,
          "short overall deadline admitted mutation");
  require(fake_now <= deadline, "overall admission deadline was extended");
  reset(); const auto cancel_at = fake_now + 5ms;
  require(!admit(fake_now + 2s, [=] { return fake_now >= cancel_at; }) && g_restore_expected && ping_calls == 0,
          "cancellation during reconnect admitted mutation");
  reset(); require(!admit(fake_now + 2s, [] { return true; }) && fast_calls == 0 && g_restore_expected,
          "already cancelled admission reached IPC");
  for (int cancel_check = 1; cancel_check <= 16; ++cancel_check) {
    reset(); first_send_ok = true;
    int checks = 0; bool observed_cancel = false;
    const bool allowed = admit(fake_now + 2s, [&] {
      observed_cancel = ++checks >= cancel_check;
      return observed_cancel;
    });
    require(!observed_cancel || !allowed, "late admission cancellation admitted mutation");
  }
  reset(); helper_live = false;
  require(admit(fake_now + 2s, [] { return false; }) && !g_restore_expected && reset_calls == 0,
          "dead helper unnecessarily blocked admission");
  reset(); (void) display_helper_integration::disarm_pending_restore({}, fake_now + 2s);
  require(reset_calls == 0 && ping_calls == 0, "background recovery reconnected");
  reset(); g_last_disarm_attempt_us = now_steady_us();
  require(admit(fake_now + 2s, [] { return false; }) && reset_calls == 1,
          "background throttle suppressed the explicit admission reconnect");
  reset(); shutting_down = true; g_restore_expected = false;
  require(!admit(fake_now + 2s, [] { return false; }) && fast_calls == 0,
          "shutdown admitted mutation or performed reconnect");
  reset(); helper_live = false; probe_delay = 20;
  require(display_helper_integration::restore_in_progress({}, fake_now + 10ms) && g_restore_expected,
          "interrupted helper liveness erased the restore guard");
  for (int probe : {1, 2}) {
    for (bool live : {false, true}) {
      for (auto event : {Interruption::Shutdown, Interruption::Cancel, Interruption::Deadline}) {
        reset(); reset_ok = false; interrupt_on_probe = probe;
        interrupted_probe_live = live; interruption = event;
        const bool allowed = admit(fake_now + 2s, [] { return cancelled_by_probe; });
        std::cout << "probe_interruption: probe=" << probe << " live=" << live
                  << " interruption=" << static_cast<int>(event) << " allowed=" << allowed
                  << " guard=" << g_restore_expected << " shutdown=" << shutting_down
                  << " probes=" << helper_probe_calls << std::endl;
        require(!allowed && g_restore_expected && killed == 0,
                "interrupted liveness admitted mutation or erased known restore guard");
        if (probe == 1) require(fast_calls == 0, "interrupted first liveness continued to DISARM");
      }
    }
  }
  // Execute both actual launch/resume predicates, including their final use
  // after restore_in_progress has returned its earlier query result.
  for (auto factory : {production_startup_cancelled_0, production_startup_cancelled_1}) {
    reset(); const auto deadline = fake_now + 2s;
    auto cancelled = factory(deadline);
    require(!cancelled(), "fresh production startup predicate was cancelled");
    shutting_down = true;
    require(cancelled(), "production startup predicate ignored shutdown");
    shutting_down = false; fake_now = deadline;
    require(cancelled(), "production startup predicate ignored deadline");
    reset(); reset_ok = false; interrupt_on_probe = 2; interrupted_probe_live = false;
    interruption = Interruption::Shutdown;
    require(!admit(fake_now + 2s, factory(fake_now + 2s)) && g_restore_expected,
            "production startup predicate admitted shutdown during final liveness");
  }
  // The absolute admission deadline remains authoritative even if a caller's
  // cancellation predicate does not include it and the query had no restore.
  reset(); first_send_ok = true;
  require(!admit(fake_now + 2s, [&] {
    if (!g_restore_expected) advance(3000);
    return false;
  }), "final admission ignored its absolute deadline after an empty restore query");
  std::cout << "15 baseline admission scenarios, 12 probe interruption boundaries, "
               "2 production startup predicates and final deadline boundary passed\n";
}
'''


def transport():
    pipes = (args.pipes_source or root / 'src/platform/windows/ipc/pipes.cpp').read_text()
    client = (args.client_source or root / 'src/platform/windows/ipc/display_settings_client.cpp').read_text()
    methods = '\n'.join(function(pipes, signature) for signature in [
        'int client_wait_ms(',
        'std::unique_ptr<INamedPipe> NamedPipeFactory::create_client(\n',
        'winrt::file_handle NamedPipeFactory::create_client_pipe(',
        'std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_client(\n',
        'std::unique_ptr<INamedPipe> AnonymousPipeFactory::handshake_client(',
        'AnonymousPipeFactory::HandshakeMessageResult AnonymousPipeFactory::receive_handshake_message(',
        'bool AnonymousPipeFactory::send_handshake_ack(',
        'std::unique_ptr<INamedPipe> AnonymousPipeFactory::connect_to_data_pipe(',
    ]).replace('std::this_thread::sleep_for(', 'fake_sleep(')
    clients = '\n'.join(function(client, signature) for signature in [
        'static int remaining_timeout_ms(const std::chrono::steady_clock::time_point &deadline)',
        'static bool ensure_connected_locked(', 'static SessionPtr connected_session_within(',
        'bool send_ping_cancellable(',
    ])
    return common + r'''
void fake_sleep(std::chrono::milliseconds delay) { fake_now += delay; }
using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
constexpr DWORD ERROR_PIPE_BUSY = 231, ERROR_FILE_NOT_FOUND = 2;
constexpr int GENERIC_READ = 1, GENERIC_WRITE = 2, OPEN_EXISTING = 3, FILE_FLAG_OVERLAPPED = 4;
namespace winrt {
  struct file_handle {
    int value = 0;
    file_handle() = default;
    explicit file_handle(int v) : value(v) {}
    explicit operator bool() const { return value > 0; }
  };
}
enum class Scenario { Success, NoControl, BusyControl, NoHandshake, SlowAck, MissingData, Inline };
Scenario scenario = Scenario::Success;
DWORD last_error = 0;
int opens = 0, ack_sends = 0, receives = 0, ping_sends = 0;
int max_receive_wait = 0, max_named_wait = 0;
ULONGLONG GetTickCount64() { return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(fake_now.time_since_epoch()).count()); }
DWORD GetLastError() { return last_error; }
int CreateFileW(const wchar_t *name, int, int, void *, int, DWORD, void *) {
  ++opens;
  const bool data = std::wstring(name).find(L"data") != std::wstring::npos;
  if (scenario == Scenario::NoControl || (scenario == Scenario::MissingData && data)) {
    last_error = ERROR_FILE_NOT_FOUND; return 0;
  }
  if (scenario == Scenario::BusyControl) { last_error = ERROR_PIPE_BUSY; return 0; }
  return data ? 2 : 1;
}
void WaitNamedPipeW(const wchar_t *, DWORD timeout) {
  max_named_wait = std::max(max_named_wait, static_cast<int>(timeout)); advance(static_cast<int>(timeout));
}
void Sleep(DWORD timeout) { advance(static_cast<int>(timeout)); }
std::wstring utf8_to_wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }
std::string wide_to_utf8(const std::wstring &s) { return std::string(s.begin(), s.end()); }
namespace platf::dxgi {
  enum class PipeResult { Success, Timeout, BrokenPipe, Error, Disconnected };
  constexpr uint8_t ACK_MSG = 2;
  struct AnonConnectMsg { wchar_t pipe_name[40]; };
  struct INamedPipe {
    virtual ~INamedPipe() = default;
    virtual bool send(std::span<const uint8_t>, int) = 0;
    virtual PipeResult receive(std::span<uint8_t>, size_t &, int) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() = 0;
    virtual void wait_for_client_connection(int) {}
  };
  struct WinPipe : INamedPipe {
    bool connected = true;
    WinPipe(winrt::file_handle, bool) {}
    bool send(std::span<const uint8_t>, int timeout) override {
      ++ack_sends;
      if (scenario == Scenario::SlowAck) { advance(timeout); return false; }
      advance(1); return true;
    }
    PipeResult receive(std::span<uint8_t> bytes, size_t &count, int timeout) override {
      ++receives; max_receive_wait = std::max(max_receive_wait, timeout);
      if (scenario == Scenario::NoHandshake) { advance(timeout); count = 0; return PipeResult::Timeout; }
      if (scenario == Scenario::Inline) {
        uint32_t size = 2; std::memcpy(bytes.data(), &size, sizeof(size)); count = sizeof(size);
      } else {
        AnonConnectMsg message {}; std::wcscpy(message.pipe_name, L"data");
        std::memcpy(bytes.data(), &message, sizeof(message)); count = sizeof(message);
      }
      advance(1); return PipeResult::Success;
    }
    void disconnect() override { connected = false; }
    bool is_connected() override { return connected; }
  };
  struct PrefetchedPipe : INamedPipe {
    std::unique_ptr<INamedPipe> inner;
    PrefetchedPipe(std::unique_ptr<INamedPipe> p, std::vector<uint8_t>) : inner(std::move(p)) {}
    explicit PrefetchedPipe(std::unique_ptr<INamedPipe> p) : inner(std::move(p)) {}
    bool send(std::span<const uint8_t> b, int t) override { return inner->send(b, t); }
    PipeResult receive(std::span<uint8_t> b, size_t &c, int t) override { return inner->receive(b,c,t); }
    void disconnect() override { inner->disconnect(); }
    bool is_connected() override { return inner && inner->is_connected(); }
  };
  using FramedPipe = PrefetchedPipe;
  struct NamedPipeFactory {
    DWORD _client_security_flags = 0;
    std::unique_ptr<INamedPipe> create_client(const std::string &, const std::function<bool()> &, Time);
    winrt::file_handle create_client_pipe(const std::wstring &, const std::function<bool()> &, Time) const;
  };
  struct AnonymousPipeFactory {
    enum class HandshakeMessageResult { Message, Inline, Failed };
    NamedPipeFactory _pipe_factory;
    std::unique_ptr<INamedPipe> create_client(const std::string &, const std::function<bool()> &, Time);
    std::unique_ptr<INamedPipe> handshake_client(std::unique_ptr<INamedPipe>, const std::function<bool()> &, Time);
    HandshakeMessageResult receive_handshake_message(std::unique_ptr<INamedPipe> &, AnonConnectMsg &, std::vector<uint8_t> &, const std::function<bool()> &, Time) const;
    bool send_handshake_ack(std::unique_ptr<INamedPipe> &, const std::function<bool()> &, Time) const;
    std::unique_ptr<INamedPipe> connect_to_data_pipe(const std::string &, const std::function<bool()> &, Time);
  };
''' + methods + r'''
  struct SelfHealingPipe {
    std::function<std::unique_ptr<INamedPipe>()> creator;
    std::unique_ptr<INamedPipe> inner;
    explicit SelfHealingPipe(std::function<std::unique_ptr<INamedPipe>()> c) : creator(std::move(c)) {}
    void wait_for_client_connection(int t) { inner = creator(); if (inner) inner->wait_for_client_connection(t); }
    bool is_connected() { return inner && inner->is_connected(); }
  };
}
namespace platf::display_helper_client {
  struct ConnectionSession {
    std::shared_ptr<platf::dxgi::SelfHealingPipe> pipe;
    std::uint64_t generation;
    ConnectionSession(std::shared_ptr<platf::dxgi::SelfHealingPipe> p, std::uint64_t g) : pipe(std::move(p)), generation(g) {}
  };
  using SessionPtr = std::shared_ptr<ConnectionSession>;
  SessionPtr &session_singleton() { static SessionPtr p; return p; }
  std::atomic<std::uint64_t> &connection_generation() { static std::atomic<std::uint64_t> g{1}; return g; }
  std::uint64_t allocate_connection_generation() { return connection_generation().load() + 1; }
  SessionPtr retire_session_locked() { auto old = session_singleton(); session_singleton().reset(); return old; }
  bool shutdown_requested() { return false; }
  void reset_log_level_cache() {}
  int effective_connect_timeout() { return 2000; }
  std::timed_mutex &connection_mutex() { static std::timed_mutex m; return m; }
  enum class MsgType { Ping };
  bool send_serialized_within(const SessionPtr &session, MsgType, const std::vector<uint8_t> &, Time deadline,
      std::optional<std::uint64_t>, std::optional<int>, std::uint64_t *, bool, const std::function<bool()> &cancelled) {
    if (!session || (cancelled && cancelled()) || fake_now >= deadline) return false;
    ++ping_sends; advance(1); return true;
  }
''' + clients + r'''
}
void reset_transport(Scenario value) {
  scenario = value; fake_now = Time{} + 100s;
  opens = ack_sends = receives = ping_sends = max_receive_wait = max_named_wait = 0;
  platf::display_helper_client::session_singleton().reset();
}
int main() {
  for (auto mode : {Scenario::NoControl, Scenario::BusyControl, Scenario::NoHandshake, Scenario::SlowAck, Scenario::MissingData}) {
    reset_transport(mode); const auto start = fake_now;
    require(!platf::display_helper_client::send_ping_cancellable(750, [] { return false; }, fake_now + 10s),
            "stalled reconnect unexpectedly reached PING");
    require(fake_now <= start + 750ms && ping_sends == 0, "native wait exceeded the absolute reconnect deadline");
  }
  reset_transport(Scenario::Success);
  require(platf::display_helper_client::send_ping_cancellable(750, [] { return false; }, fake_now + 10s) &&
          ack_sends == 1 && ping_sends == 1 && platf::display_helper_client::session_singleton(),
          "healthy anonymous reconnect failed");
  reset_transport(Scenario::Inline);
  require(platf::display_helper_client::send_ping_cancellable(750, [] { return false; }, fake_now + 10s) &&
          ack_sends == 0 && ping_sends == 1, "legacy inline connection failed");
  for (auto mode : {Scenario::NoControl, Scenario::BusyControl, Scenario::NoHandshake, Scenario::MissingData}) {
    reset_transport(mode); const auto start = fake_now; const auto cancel_at = fake_now + 75ms;
    require(!platf::display_helper_client::send_ping_cancellable(750, [=] { return fake_now >= cancel_at; }, fake_now + 10s),
            "cancelled transport reached PING");
    require(fake_now <= start + 125ms && ping_sends == 0, "cancellation was not observed between 50ms wait slices");
  }
  reset_transport(Scenario::NoHandshake); const auto short_start = fake_now;
  require(!platf::display_helper_client::send_ping_cancellable(750, [] { return false; }, fake_now + 120ms) &&
          fake_now <= short_start + 120ms, "short overall startup deadline was extended");
  reset_transport(Scenario::Success);
  require(!platf::display_helper_client::send_ping_cancellable(750, [] { return true; }, fake_now + 10s) && opens == 0,
          "already cancelled transport opened a pipe");
  std::cout << "13 production transport scenarios passed\n";
}
'''


def resume():
    nvhttp = (args.nvhttp_source or root / 'src/nvhttp.cpp').read_text()
    start = nvhttp.index('      if (!allow_display_changes) {')
    brace = nvhttp.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (nvhttp[end] == '{') - (nvhttp[end] == '}')
        end += 1
    block = nvhttp[start:end].replace('std::chrono::steady_clock::now()', 'clock_now()')
    admission = nvhttp.index('const bool virtual_display_mutation_allowed =', end)
    refresh = nvhttp.rindex('apply_framegen_refresh_policy(request_virtual_display);', end, admission)
    guard = nvhttp.rfind('if (allow_display_changes', end, refresh)
    call = nvhttp.index('apply_virtual_display_request(request_virtual_display);', admission)
    if guard >= 0:
        # Include the actual enclosing creation gate and its else branch. This
        # catches the downstream regression that only set recreated_on_demand
        # but subsequently skipped creation when disconnect revert was off.
        brace = nvhttp.index('{', guard)
        depth, tail_end = 1, brace + 1
        while depth:
            depth += (nvhttp[tail_end] == '{') - (nvhttp[tail_end] == '}')
            tail_end += 1
        next_else = re.match(r'\s*else\s*\{', nvhttp[tail_end:])
        if next_else:
            depth = 1
            tail_end += next_else.end()
            while depth:
                depth += (nvhttp[tail_end] == '{') - (nvhttp[tail_end] == '}')
                tail_end += 1
        tail = nvhttp[guard:tail_end]
    else:
        tail = nvhttp[refresh:call + len('apply_virtual_display_request(request_virtual_display);')]
    tail = tail.replace('std::chrono::steady_clock::now()', 'clock_now()')
    return common + r'''
struct ResumeSession {
  bool virtual_display = false, virtual_display_failed = false;
  bool virtual_display_needs_resume_apply = false, virtual_display_recreated_on_demand = false;
  std::string virtual_display_device_id, client_name;
  std::array<uint8_t, 16> virtual_display_guid_bytes {};
  std::optional<Time> virtual_display_ready_since;
  std::optional<bool> virtual_display_hdr_enabled;
  unsigned long long normal_vdd_identity_token = 1;
};
namespace config { std::string output; void set_runtime_output_name_override(const std::string &v) { output = v; } }
namespace VDISPLAY {
  std::optional<std::string> existing;
  bool adapter_matches = true;
  std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(const std::string &, const std::string &, const std::string &, bool) { return existing; }
  bool configuredRenderAdapterMatchesVirtualDisplay(const std::string &, const std::string &) { return adapter_matches; }
  namespace policy {
    bool allow_generic_resume_fallback() { return false; }
    enum class retained_resume_reuse_path_e { prepared_existing_display };
  }
}
template<class... T> void stage_retained_rtsp_virtual_display_recovery(T &&...) {}
namespace display_helper_integration {
  bool disarm_pending_restore(const std::function<bool()> &, Time, bool = false) { return true; }
  bool restore_in_progress(const std::function<bool()> &, Time) { return false; }
  namespace request_policy {
    bool supersede_restore_for_virtual_display(const std::function<void()> &disarm, const std::function<bool()> &restoring) {
      disarm(); return !restoring();
    }
  }
}
int creation_entries = 0;
bool identity_available = true;
void prepare_resume(ResumeSession &session, bool request_virtual_display) {
  ResumeSession *launch_session = &session;
  const bool allow_display_changes = false, shared_virtual_display_mode = false;
  bool retained_resume_recovery_rearm = false;
  std::string virtual_display_stable_id, virtual_display_stable_guid;
  std::optional<std::string> pending_output_override, app_output_override;
  const bool include_identity_log_fields = true;
  auto reserve_normal_vdd_identity = [] { return identity_available; };
  auto apply_framegen_refresh_policy = [](bool) {};
  const auto display_startup_deadline = fake_now + 2s;
  const std::function<bool()> display_startup_cancelled = [] { return false; };
  [[maybe_unused]] const std::function<void()> disarm_startup_restore = [&] {
    (void) display_helper_integration::disarm_pending_restore(display_startup_cancelled, display_startup_deadline, true);
  };
  auto apply_virtual_display_request = [&](bool requested) { if (requested) { ++creation_entries; session.virtual_display = true; } };
''' + block + '\n' + tail + r'''
}
int main() {
  ResumeSession missing;
  prepare_resume(missing, true);
  require(missing.virtual_display_recreated_on_demand && creation_entries == 1,
          "missing resume display did not continue to creation with revert-on-disconnect disabled");
  creation_entries = 0; VDISPLAY::existing = "DISPLAY10";
  ResumeSession existing; prepare_resume(existing, true);
  require(existing.virtual_display && existing.virtual_display_needs_resume_apply &&
          existing.virtual_display_device_id == "DISPLAY10" && creation_entries == 0,
          "existing resume display was recreated or lost its capture target");
  VDISPLAY::adapter_matches = false;
  ResumeSession wrong_adapter; prepare_resume(wrong_adapter, true);
  require(wrong_adapter.virtual_display_recreated_on_demand && creation_entries == 1,
          "wrong-adapter resume display did not continue to recreation");
  creation_entries = 0;
  ResumeSession physical; prepare_resume(physical, false);
  require(!physical.virtual_display_recreated_on_demand && creation_entries == 0,
          "physical resume unexpectedly mutated virtual displays");
  identity_available = false;
  ResumeSession denied; prepare_resume(denied, true);
  require(!denied.virtual_display_recreated_on_demand && creation_entries == 0,
          "failed identity reservation continued to creation");
  std::cout << "5 production resume scenarios passed\n";
}
'''


def target_entry():
    """Run the authorized ordinary target launch entry chain with OS fakes.

    Exact production classification, startup/preflight/session prefix, preparation
    call and pre-APPLY prefix are retained. Unrelated HTTP/config/app collaborators
    are omitted; preparation's large non-input body is the inherited actual
    admission/mutation fixture. This is not a full HTTP or native launch.
    """
    nvhttp = (args.nvhttp_source or root / 'src/nvhttp.cpp').read_text()
    classification = statement(nvhttp, 'bool is_input_only = config::input.enable_input_only_mode')
    begin = nvhttp.index('const auto display_startup_deadline =', nvhttp.index(classification))
    end = nvhttp.index('request_audio_hold.attach(launch_session->audio_context_guard);', begin)
    entry = nvhttp[begin:end + len('request_audio_hold.attach(launch_session->audio_context_guard);')]
    prepare = statement(nvhttp, 'prepare_virtual_display_for_session(', end)
    input_match = re.search(r'if \([^)]*is_input_only[^)]*\) \{', nvhttp[nvhttp.index('void prepare_virtual_display_for_session('):])
    input_begin = nvhttp.index('void prepare_virtual_display_for_session(') + input_match.start()
    input_end = nvhttp.index('} else {', input_begin)
    input_guard = nvhttp[input_begin:input_end + len('} else {')]
    apply_begin = nvhttp.index('if (', nvhttp.index('// The display should be restored in case something fails', end))
    apply_end = nvhttp.index('// Apply a per-client HDR profile to physical displays', apply_begin)
    preapply = nvhttp[apply_begin:apply_end] + '\n#endif\n}\n'
    body = admission(False).split('int main() {', 1)[0]
    body += r"""
#define _WIN32
namespace config {
  struct Input { bool enable_input_only_mode = true; } input;
  int video = 0;
}
namespace proc { constexpr int input_only_app_id = 12345; }
constexpr const char *REMOTE_INPUT_UUID = "remote-input-synthetic-role";
namespace video { using encoder_probe_adapter_hint_lease_t = int; }
namespace VDISPLAY::policy { using retained_resume_recovery_rearm_t = int; }
namespace rtsp_stream {
  struct launch_session_t {
    bool input_only = false, display_config_preapply_attempted = false, display_config_preapplied = false;
    std::shared_ptr<void> audio_context_guard;
  };
}
int disabled_requests = 0, creation_mutations = 0, preapply_requests = 0, apply_calls = 0;
Time entry_deadline {}, apply_deadline {};
bool entry_input_only = false;
namespace dispatch { bool include_request_identity_log_fields(int) { return true; } }
std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session_from_snapshot(
  int, bool input_only, int, int, const int *, int, bool, bool) {
  require(reset_calls == 1 || shutting_down || fake_now >= entry_deadline,
          "ordinary session construction bypassed unconditional production preflight");
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->input_only = input_only;
  return session;
}
namespace display_helper_integration {
  constexpr auto kStreamStartApplyVerificationTimeout = 2s;
  enum class ApplyRetryPolicy { StreamStart };
  namespace helpers {
    std::optional<int> build_request_from_session(int, const rtsp_stream::launch_session_t &, bool) {
      ++preapply_requests;
      return 1;
    }
  }
  bool apply(int, std::nullptr_t, const std::function<bool()> &, ApplyRetryPolicy, Time deadline) {
    ++apply_calls; apply_deadline = deadline; return true;
  }
}
void prepare_virtual_display_for_session(
  const std::shared_ptr<rtsp_stream::launch_session_t> &,
  audio::display_change_hold_t &, bool, bool,
  bool is_input_only, std::optional<std::string> &,
  std::optional<video::encoder_probe_adapter_hint_lease_t> &,
  VDISPLAY::policy::retained_resume_recovery_rearm_t *,
  const std::function<void()> &disarm_startup_restore,
  const std::function<bool()> &display_startup_cancelled, Time display_startup_deadline, bool, bool) {
  entry_deadline = display_startup_deadline;
  auto disable_virtual_display_request = [] { ++disabled_requests; };
""" + input_guard + r"""
    production_mutation(display_startup_deadline, display_startup_cancelled,
                        disarm_startup_restore, creation_mutations);
  }
}
void production_ordinary_launch_entry(int appid, const std::string &appuuid_str) {
  audio::display_change_hold_t request_audio_hold {[] { return std::shared_ptr<void>{}; }};
  int host_audio = 0, args = 0, verified_client = 0, request_client_identity = 0, request = 0, trace_id = 0;
  bool no_active_sessions = true;
  [[maybe_unused]] bool revert_display_configuration = false;
""" + classification + r"""
  entry_input_only = is_input_only;
  entry_deadline = fake_now + display_helper_integration::kStreamStartApplyVerificationTimeout;
#ifdef _WIN32
""" + entry.replace('std::chrono::steady_clock::now()', 'clock_now()') + r"""
  std::optional<std::string> pending_output_override;
  std::optional<video::encoder_probe_adapter_hint_lease_t> pending_adapter_hint;
""" + prepare + preapply + r"""
}
int main() {
  for (bool input_only : {true, false}) {
    for (bool recent : {true, false}) {
      for (int failure : {0, 1, 2, 3}) {
        reset();
        disabled_requests = creation_mutations = preapply_requests = apply_calls = 0;
        entry_deadline = apply_deadline = {};
        if (recent) g_last_revert_us = now_steady_us();
        if (failure == 1) reset_ok = false;
        if (failure == 2) ping_ok = false;
        if (failure == 3) second_send_ok = false;
        production_ordinary_launch_entry(input_only ? proc::input_only_app_id : 23456, "configured-app-uuid");
        std::cout << "target_ordinary_entry: numeric_input_only=" << input_only << " recent=" << recent
                  << " failure=" << failure << " classified=" << entry_input_only
                  << " reset=" << reset_calls << " killed=" << killed << " guard=" << g_restore_expected
                  << " disable=" << disabled_requests << " mutation=" << creation_mutations
                  << " preapply=" << preapply_requests << " apply=" << apply_calls << std::endl;
        require(entry_input_only == input_only, "actual configured numeric input-only classification changed");
        require(reset_calls == 1 && killed == 0 && g_restore_expected == (failure != 0),
                "actual entry preflight skipped recovery or killed/lost a known live restore guard");
        require(disabled_requests == (input_only ? 1 : 0), "actual preparation input-only guard changed");
        require(creation_mutations == (!input_only && failure == 0 ? 1 : 0),
                "entry VDD route admitted failed recovery or input-only mutation");
        require(preapply_requests == (input_only ? 0 : 1) && apply_calls == (input_only ? 0 : 1),
                "actual launch pre-APPLY condition admitted input-only or lacked ordinary positive control");
        require(input_only || apply_deadline == entry_deadline, "pre-APPLY renewed the original absolute deadline");
      }
    }
  }
  std::cout << "16 actual ordinary target entry-chain cases passed (8 configured numeric input-only, 8 normal controls)\n";
}
"""
    return body


with tempfile.TemporaryDirectory(prefix='virtual-display-startup-') as temporary:
    directory = pathlib.Path(temporary)
    for name, factory in [('admission', admission), ('preflight', lambda: admission(True)), ('transport', transport), ('resume', resume), ('target-entry', target_entry)]:
        if args.fixture not in [name, 'all']:
            continue
        if name == 'target-entry' and args.fixture == 'all' and 'bool is_input_only = config::input.enable_input_only_mode' not in (args.nvhttp_source or root / 'src/nvhttp.cpp').read_text():
            continue
        source, binary = directory / f'{name}.cpp', directory / name
        source.write_text(factory())
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

"""Execute both helpers' notification and idle-failure liveness methods.

These compile the production methods with deterministic collaborators. Native
Windows IPC and display qualification remain separate.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
legacy = (root / "tools/display_settings_helper.cpp").read_text()
v2 = (root / "src/platform/windows/display_helper_v2/state_machine.cpp").read_text()


def function(source, signature):
    match = re.search(r"\s*".join(map(re.escape, signature.split())), source)
    assert match, signature
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


notify = function(v2, "void StateMachine::notify_recovery_status()")
wire = function(v2, "RecoveryStatus to_wire_status(recovery_status::status status)")
legacy_notify = function(legacy, "void notify_recovery_status_locked()")
legacy_publish = function(legacy, "void publish_recovery_status(")
legacy_heartbeat = function(legacy, "bool check_heartbeat_timeout()")

program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>
#include "src/platform/windows/recovery_status.h"
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
using namespace display_helper;
using RecoveryStatus = recovery_status::status;
using Notification = std::tuple<std::uint64_t, RecoveryStatus, std::uint64_t, bool, std::uint64_t>;
struct StateMachine {
  recovery_status::policy recovery_status_policy_;
  std::function<void(std::uint64_t, RecoveryStatus, std::uint64_t, bool, std::uint64_t)> recovery_status_result_callback_;
  std::function<std::uint64_t()> connection_epoch_provider_;
  std::uint64_t current_connection_epoch_ = 3;
  struct System { std::uint64_t generation = 9; std::uint64_t current_generation() { return generation; } } system_;
  bool worker = false, staged_state_reset_pending_ = false;
  std::vector<int> deferred_mutation_commands_;
  bool mutation_worker_active() { return worker; }
  std::optional<std::tuple<std::uint64_t, RecoveryStatus, std::uint64_t, std::uint64_t>> last_recovery_notification_;
  void notify_recovery_status();
};
''' + wire + "\n" + notify + r'''
struct ServiceState {
  recovery_status::policy recovery_status;
  std::mutex recovery_status_mutex;
  std::function<void(std::uint64_t, display_helper::recovery_status::status, std::uint64_t)> recovery_notification;
  std::atomic<std::int64_t> recovery_status_failed_at_ms {0};
  std::atomic<std::uint64_t> restore_cancel_generation {9};
  std::uint64_t epoch = 3;
  std::uint64_t current_connection_epoch() const { return epoch; }
  std::int64_t now = 1;
  std::int64_t steady_now_ms() { return now; }
  std::atomic<bool> heartbeat_monitor_active {true}, heartbeat_revert_armed {false};
  std::atomic<std::int64_t> heartbeat_optional_until_ms {0}, last_heartbeat_ms {0}, heartbeat_revert_deadline_ms {0};
  static constexpr auto kHeartbeatMissWindow = std::chrono::seconds {30};
  static constexpr auto kHeartbeatRecoveryWindow = std::chrono::minutes {2};
''' + legacy_notify + "\n" + legacy_publish + "\n" + legacy_heartbeat + r'''
};

int main() {
  StateMachine fsm;
  std::vector<Notification> notifications;
  fsm.recovery_status_result_callback_ = [&](auto ticket, auto status, auto event, auto parked, auto epoch) {
    notifications.emplace_back(ticket, status, event, parked, epoch);
  };
  fsm.notify_recovery_status();
  assert(notifications.empty());
  fsm.recovery_status_policy_.begin(7, 9, 3);
  fsm.notify_recovery_status();
  assert(notifications.size() == 1 && std::get<1>(notifications.back()) == RecoveryStatus::active);
  fsm.notify_recovery_status(); // Idle ticks cannot generate periodic traffic.
  assert(notifications.size() == 1);
  fsm.recovery_status_policy_.observe_event();
  fsm.worker = true;
  fsm.recovery_status_policy_.publish(RecoveryStatus::failed, 7, 9, 3);
  fsm.notify_recovery_status();
  assert(notifications.size() == 2 && std::get<1>(notifications.back()) == RecoveryStatus::active);
  assert(std::get<2>(notifications.back()) == 1);
  fsm.worker = false;
  fsm.notify_recovery_status(); // Completion publishes without another event or query.
  assert(notifications.size() == 3 && std::get<1>(notifications.back()) == RecoveryStatus::failed);
  assert(!std::get<3>(notifications.back()) && !fsm.recovery_status_policy_.parked());
  fsm.notify_recovery_status();
  assert(notifications.size() == 3);
  fsm.system_.generation = 10;
  fsm.notify_recovery_status();
  assert(std::get<1>(notifications.back()) == RecoveryStatus::unknown);
  fsm.recovery_status_policy_.supersede();
  const auto previous_count = notifications.size();
  fsm.notify_recovery_status();
  assert(notifications.size() == previous_count);

  ServiceState legacy;
  unsigned sent = 0;
  legacy.recovery_notification = [&](auto ticket, auto status, auto event) {
    assert(ticket == 7 && status == RecoveryStatus::failed && event == 0);
    ++sent;
  };
  legacy.recovery_status.begin(7, 9, 3);
  legacy.publish_recovery_status(RecoveryStatus::failed, 7, 9, 3);
  assert(sent == 1);
  legacy.now += 24 * 60 * 60 * 1000; // No status polling is needed to retain this failed ticket.
  assert(!legacy.check_heartbeat_timeout());
  assert(!legacy.heartbeat_revert_armed.load());
  legacy.publish_recovery_status(RecoveryStatus::failed, 8, 9, 3);
  assert(sent == 1); // Stale ticket cannot publish a completion.
  legacy.epoch = 4;
  legacy.notify_recovery_status_locked();
  assert(sent == 1); // Never route a retired session's failure to a new pipe.
  assert(!legacy.check_heartbeat_timeout() && legacy.heartbeat_revert_armed.load());
  legacy.now += 120000;
  assert(legacy.check_heartbeat_timeout()); // Normal disconnect recovery still operates.
  legacy.recovery_notification = {};
  legacy.epoch = 3;
  legacy.notify_recovery_status_locked();
  assert(sent == 1); // Cleared callback cannot use a destroyed pipe.
}
'''

with tempfile.TemporaryDirectory(prefix="recovery-event-notifications-") as temporary:
    source = pathlib.Path(temporary) / "test.cpp"
    binary = pathlib.Path(temporary) / "test"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

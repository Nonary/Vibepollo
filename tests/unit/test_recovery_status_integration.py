"""Exercise the production v2 recovery-status park handler.

Neighboring tests execute the production notification methods, legacy query
transport, shared status policy, and heartbeat monitor. This harness adds the
production v2 StateMachine command handler, where status, liveness, and park
acknowledgement interact. Companion protocol and lifecycle harnesses execute
the production wire parsers, serializers, and legacy event-pump paths.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++"
fsm_path = root / "src/platform/windows/display_helper_v2/state_machine.cpp"


def function(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(map(re.escape, tokens)), source)
    if match is None:
        raise AssertionError(f"Production function not found: {signature}")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


fsm = fsm_path.read_text()
status_handler = function(fsm, "void StateMachine::handle_recovery_status_command(const RecoveryStatusCommand &command)")
message_handler = function(fsm, "void StateMachine::handle_message(const Message &message)")
notify_status = function(fsm, "void StateMachine::notify_recovery_status()")
validation_handler = function(fsm, "void StateMachine::handle_recovery_validation_completed(const RecoveryValidationCompleted &completed)")
wire_status = function(fsm, "RecoveryStatus to_wire_status(recovery_status::status status)")

program = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <type_traits>
#include <vector>
#include "src/platform/windows/recovery_status.h"

namespace display_helper::v2 {
  using RecoveryStatus = recovery_status::status;
  struct RecoveryStatusCommand {
    std::uint64_t ticket = 0;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
    bool park = false;
  };
  struct ApplyCommand {}; struct RevertCommand {}; struct DisarmCommand {};
  struct ExportGoldenCommand {}; struct SnapshotCurrentCommand {}; struct RefreshRateCommand {};
  struct ResetCommand {}; struct PingCommand {}; struct StopCommand {}; struct ApplyCompleted {};
  struct VerificationCompleted {}; struct ResetCompleted {}; struct RefreshRateCompleted {};
  struct RecoveryCompleted {}; struct RecoveryValidationCompleted { bool success = true; std::uint64_t generation = 1; };
  struct DisplayEventMessage {}; struct HelperEventMessage {};
  using Message = std::variant<ApplyCommand, RevertCommand, DisarmCommand, ExportGoldenCommand,
    SnapshotCurrentCommand, RefreshRateCommand, ResetCommand, PingCommand, StopCommand,
    RecoveryStatusCommand, ApplyCompleted, VerificationCompleted, ResetCompleted,
    RefreshRateCompleted, RecoveryCompleted, RecoveryValidationCompleted,
    DisplayEventMessage, HelperEventMessage>;
  enum class State { EventLoop, Busy, Waiting };
  enum class ApplyAction { Revert }; enum class ApplyStatus { Ok };
  enum class MutationWorkerKind { Recovery };
  using DeferredMutationCommand = std::variant<ResetCommand, ApplyCommand>;
  constexpr auto kRecoveryFeedbackQuietPeriod = std::chrono::milliseconds(1000);
  struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
  struct FakeSystem {
    std::uint64_t generation = 1;
    unsigned liveness_pings = 0;
    std::uint64_t current_generation() const { return generation; }
    void record_liveness_ping() { ++liveness_pings; }
    void disarm_heartbeat() {} void refresh_shell() {} void delete_restore_task() {}
    auto now() const { return std::chrono::steady_clock::now(); }
  };
  struct FakeScheduler {
    unsigned disarms = 0;
    void disarm() { ++disarms; }
    void on_attempt_failed(std::chrono::steady_clock::time_point) {}
  };
  struct Result {
    std::uint64_t ticket;
    RecoveryStatus status;
    std::uint64_t event;
    bool parked;
    std::uint64_t epoch;
  };
  class StateMachine {
  public:
    recovery_status::policy recovery_status_policy_;
    FakeSystem system_;
    FakeScheduler scheduler_;
    State state_ = State::EventLoop;
    bool worker_active = false;
    bool staged_state_reset_pending_ = false;
    std::deque<DeferredMutationCommand> deferred_mutation_commands_;
    std::optional<int> active_mutation_worker_ {1}, recovery_baseline_present_devices_;
    bool recovery_armed_ = true, display_changes_pending_recovery_ = true;
    bool unconfirmed_cancelled_mutation_ = false, restore_task_created_ = true;
    bool staged_state_prepared_ = false, exit_after_staged_state_reset_ = false;
    std::optional<bool> recovery_staged_state_reset_succeeded_;
    std::optional<std::chrono::steady_clock::time_point> recovery_event_feedback_quiet_until_;
    struct RestoreState { void reset_request_progress() {} } restore_state_;
    std::function<void(int)> exit_callback_;
    bool owns_active_worker(MutationWorkerKind, std::uint64_t) { return active_mutation_worker_.has_value(); }
    bool is_stale(std::uint64_t generation) { return generation != system_.generation; }
    void retain_recovery_after_cancelled_recovery() {}
    void drain_deferred_mutation_commands() {}
    void transition(State state, ApplyAction, ApplyStatus = ApplyStatus::Ok) { state_ = state; }
    void begin_staged_state_reset() { staged_state_reset_pending_ = true; }

    std::optional<int> deferred_recovery_display_event_;
    std::uint64_t current_connection_epoch_ = 1;
    std::function<std::uint64_t()> connection_epoch_provider_;
    std::optional<std::tuple<std::uint64_t, RecoveryStatus, std::uint64_t, std::uint64_t>> last_recovery_notification_;
    std::vector<Result> results;
    std::function<void(std::uint64_t, RecoveryStatus, std::uint64_t, bool, std::uint64_t)> recovery_status_result_callback_;
    bool mutation_worker_active() const { return worker_active; }
    bool is_stale_connection(std::uint64_t epoch) const { return epoch != current_connection_epoch_; }
    template<class T> void handle_apply_command(const T &) {}
    template<class T> void handle_revert_command(const T &) {}
    template<class T> void handle_disarm_command(const T &) {}
    template<class T> void handle_export_golden(const T &) {}
    template<class T> void handle_snapshot_current(const T &) {}
    template<class T> void handle_refresh_rate_command(const T &) {}
    template<class T> void handle_reset_command(const T &) {}
    template<class T> void handle_ping_command(const T &) {}
    template<class T> void handle_stop_command(const T &) {}
    template<class T> void handle_apply_completed(const T &) {}
    template<class T> void handle_verification_completed(const T &) {}
    template<class T> void handle_reset_completed(const T &) {}
    template<class T> void handle_refresh_rate_completed(const T &) {}
    template<class T> void handle_recovery_completed(const T &) {}
    void handle_recovery_validation_completed(const RecoveryValidationCompleted &);
    template<class T> void handle_display_event(const T &) {}
    template<class T> void handle_helper_event(const T &) {}
    void handle_message(const Message &message);
    void notify_recovery_status();
    void handle_recovery_status_command(const RecoveryStatusCommand &command);
  };
  RecoveryStatus to_wire_status(recovery_status::status status);
''' + wire_status + "\n" + status_handler + "\n" + notify_status + "\n" + message_handler + "\n" + validation_handler + r'''

  void initialize_machine(StateMachine &machine) {
    machine.recovery_status_result_callback_ = [&machine](auto ticket, auto status, auto event, auto parked, auto epoch) {
      machine.results.push_back({ticket, status, event, parked, epoch});
    };
    machine.recovery_status_policy_.begin(17, 1, 1);
    machine.recovery_status_policy_.observe_event();
    machine.recovery_status_policy_.publish(RecoveryStatus::failed, 17, 1, 1);
  }

  void test_active_work_never_acks_park() {
    StateMachine machine;
    initialize_machine(machine);
    machine.worker_active = true;
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.size() == 1);
    assert(machine.results.back().status == RecoveryStatus::active && !machine.results.back().parked);
    assert(machine.system_.liveness_pings == 1 && machine.scheduler_.disarms == 0);

    machine.worker_active = false;
    machine.deferred_mutation_commands_.push_back(ResetCommand{});
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.back().status == RecoveryStatus::active && !machine.results.back().parked);
    assert(machine.scheduler_.disarms == 0);
  }

  void test_idle_failure_is_parked_and_acknowledged() {
    StateMachine machine;
    initialize_machine(machine);
    machine.deferred_recovery_display_event_ = 1;
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.size() == 1);
    assert(machine.results.back().status == RecoveryStatus::failed && machine.results.back().parked);
    assert(machine.results.back().event == 1 && machine.results.back().epoch == 1);
    assert(machine.recovery_status_policy_.parked());
    assert(machine.scheduler_.disarms == 1 && !machine.deferred_recovery_display_event_);
    assert(machine.system_.liveness_pings == 1);
  }

  void test_park_requires_event_loop_and_live_ticket() {
    StateMachine machine;
    initialize_machine(machine);
    machine.state_ = State::Busy;
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.back().status == RecoveryStatus::active && !machine.results.back().parked);
    assert(!machine.recovery_status_policy_.parked() && machine.scheduler_.disarms == 0);

    machine.state_ = State::EventLoop;
    machine.system_.generation = 2;
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.back().status == RecoveryStatus::unknown && !machine.results.back().parked);
    assert(!machine.recovery_status_policy_.parked() && machine.scheduler_.disarms == 0);

    machine.system_.generation = 1;
    machine.current_connection_epoch_ = 2;
    machine.handle_recovery_status_command({17, 1, 1, true});
    assert(machine.results.back().status == RecoveryStatus::unknown && !machine.results.back().parked);
    assert(machine.system_.liveness_pings == 2); // Stale session did not count as a liveness ping.
  }

  void test_completed_restore_is_published_before_exit_retires_generation() {
    StateMachine machine;
    initialize_machine(machine);
    machine.recovery_status_policy_.restart(1, 1);
    machine.exit_callback_ = [&](int code) {
      assert(code == 0);
      assert(!machine.results.empty() && machine.results.back().status == RecoveryStatus::restored);
      assert(machine.state_ == State::Waiting && !machine.recovery_armed_);
      ++machine.system_.generation; // The production exit callback cancels operations.
    };
    machine.handle_message(Message{RecoveryValidationCompleted{true, 1}});
    assert(machine.results.front().status == RecoveryStatus::restored);
    assert(machine.system_.generation == 2);

    StateMachine failed;
    initialize_machine(failed);
    failed.handle_message(Message{RecoveryValidationCompleted{false, 1}});
    assert(failed.results.back().status == RecoveryStatus::failed);
    StateMachine stale;
    initialize_machine(stale);
    stale.handle_message(Message{RecoveryValidationCompleted{true, 2}});
    assert(stale.results.back().status != RecoveryStatus::restored);
  }

  void test_message_dispatch_publishes_status_notification() {
    StateMachine machine;
    initialize_machine(machine);
    machine.handle_message(Message{RecoveryStatusCommand{17, 1, 1, false}});
    assert(machine.results.size() == 2);
    assert(machine.results[0].status == RecoveryStatus::failed && !machine.results[0].parked);
    assert(machine.results[1].status == RecoveryStatus::failed && !machine.results[1].parked);
  }
}

int main() {
  using namespace display_helper::v2;
  test_active_work_never_acks_park();
  test_idle_failure_is_parked_and_acknowledged();
  test_park_requires_event_loop_and_live_ticket();
  test_message_dispatch_publishes_status_notification();
  test_completed_restore_is_published_before_exit_retires_generation();
}
'''

with tempfile.TemporaryDirectory(prefix="recovery-status-integration-") as temporary:
    source = pathlib.Path(temporary) / "state_machine_test.cpp"
    binary = pathlib.Path(temporary) / "state_machine_test"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

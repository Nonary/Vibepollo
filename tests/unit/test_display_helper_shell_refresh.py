"""Exercise shell-refresh ordering and check the legacy helper wiring."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = sys.argv[1] if len(sys.argv) > 1 else "g++"
source = (root / "tools/display_settings_helper.cpp").read_text()
apply = source[source.index("  bool handle_apply("):source.index("  void handle_revert(")]
frame = source[source.index("  void handle_frame("):source.index("  void attempt_revert_after_disconnect(")]
adjuncts = source[source.index("    bool run_initial_apply_adjuncts("):source.index("  };", source.index("    bool run_initial_apply_adjuncts("))]
assert source.count("refresh_shell_after_display_change(") == 1, "all refresh paths must use the worker"
assert "refresh_shell_after_display_change(" not in adjuncts
assert "state.best_effort_apply_last_cfg(false)" in apply
assert apply.index("state.controller.apply(cfg, sunshine_topology)") < apply.index("refresh_shell = true") < apply.index("run_initial_apply_adjuncts(")
assert frame.index("handle_apply(") < frame.index("MsgType::ApplyResult, result_payload") < frame.index("state.shell_refresh.request()")
lines = source.splitlines()
snapshot_calls = [i for i, line in enumerate(lines) if "(void) controller.apply_snapshot(" in line]
assert len(snapshot_calls) == 4
assert all(lines[i + 1].strip() == "shell_refresh.request();" for i in snapshot_calls)
assert "shell_refresh.cancel_pending()" not in source
assert "if (success) self->shell_refresh.request();" in source
assert "if (refresh_shell) shell_refresh.request();" in source

program = r"""
#include "tools/display_helper_shell_refresh_worker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

int main() {
  // The first callback captures old dimensions, then pauses as if blocked in
  // a broadcast. Restore completes and requests its final refresh. Destroying
  // the worker models restore-mode helper exit: pending work must be drained.
  std::promise<void> old_sampled, release_old;
  auto gate = release_old.get_future().share();
  std::atomic<int> topology {1}, calls {0};
  std::mutex mutex;
  std::vector<int> messages;
  auto callback = [&](std::stop_token) {
    const int sampled = topology.load();
    if (++calls == 1) { old_sampled.set_value(); gate.wait(); }
    std::lock_guard lock(mutex);
    messages.push_back(sampled);
  };
  auto worker = std::make_unique<display_helper::shell_refresh_worker>(callback);
  worker->request();
  assert(old_sampled.get_future().wait_for(2s) == std::future_status::ready);
  topology = 2;
#ifdef LEGACY_CONTROL
  // The old production path sends restore's refresh independently.
  callback({});
#else
  // Requests return even while the older broadcast is blocked and coalesce.
  worker->request(); worker->request(); worker->request();
#endif
  release_old.set_value();
  worker.reset();
  assert(messages.size() == 2);
  return messages.back() == 2 ? 0 : 1;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / "worker.cpp"
    cpp.write_text(program)
    for control in (False, True):
        exe = Path(tmp) / ("control" if control else "worker")
        command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), str(cpp), "-o", str(exe)]
        if control:
            command.append("-DLEGACY_CONTROL")
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], timeout=10)
        assert result.returncode == (1 if control else 0), (control, result.returncode)
print("shell refresh ordering passed; independent restore-broadcast control failed as expected")


def extract_method(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


# Compile the production destructor and cancellation method, not a rewritten
# version. Fakes supply only the unrelated event/command collaborators.
destructor = extract_method("~ServiceState()")
cancel_delayed = extract_method("void cancel_delayed_reapply()")
assert "cancel_delayed_reapply();" in destructor
lifetime_program = r"""
#include "tools/display_helper_shell_refresh_worker.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

struct Shared {
  std::atomic<bool> alive {true}, touched_dead {false};
  std::atomic<int> refreshes {0};
  std::promise<void> entered, stopped, release;
};
struct CheckedRefresh {
  Shared &shared;
  display_helper::shell_refresh_worker worker;
  explicit CheckedRefresh(Shared &s): shared(s), worker([&s](std::stop_token) { ++s.refreshes; }) {}
  ~CheckedRefresh() { shared.alive = false; }
  void request() { worker.request(); }
};
struct ServiceState {
  std::jthread delayed_reapply_thread;
  std::mutex delayed_reapply_mutex;
  CheckedRefresh shell_refresh;
  std::atomic<bool> command_worker_stop {false};
  std::condition_variable command_queue_cv;
  std::jthread command_worker;
  explicit ServiceState(Shared &s): shell_refresh(s) {}
  void stop_restore_polling() {}
  CANCEL_DELAYED
#ifndef LEGACY_LIFETIME_CONTROL
  DESTRUCTOR
#endif
};
int main() {
  Shared shared;
  auto entered = shared.entered.get_future();
  auto stopped = shared.stopped.get_future();
  auto release = shared.release.get_future().share();
  auto state = std::make_unique<ServiceState>(shared);
  auto *ptr = state.get();
  state->delayed_reapply_thread = std::jthread([&](std::stop_token stop) {
    std::stop_callback on_stop(stop, [&] { shared.stopped.set_value(); });
    shared.entered.set_value();
    release.wait(); // Represents controller.apply() returning after shutdown starts.
    if (!shared.alive.load()) { shared.touched_dead = true; return; }
    ptr->shell_refresh.request();
  });
  assert(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  std::thread release_after_stop([&] {
    assert(stopped.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    shared.release.set_value();
  });
  state.reset();
  release_after_stop.join();
#ifdef LEGACY_LIFETIME_CONTROL
  return shared.touched_dead ? 1 : 0;
#else
  assert(!shared.touched_dead);
  assert(shared.refreshes == 1); // Worker drained the delayed producer's request.
  return 0;
#endif
}
""".replace("CANCEL_DELAYED", cancel_delayed).replace("DESTRUCTOR", destructor)
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / "lifetime.cpp"
    cpp.write_text(lifetime_program)
    for control in (False, True):
        exe = Path(tmp) / ("old_lifetime" if control else "lifetime")
        command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), str(cpp), "-o", str(exe)]
        if control:
            command.append("-DLEGACY_LIFETIME_CONTROL")
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], timeout=10)
        assert result.returncode == (1 if control else 0), (control, result.returncode)
print("producer shutdown ordering passed; old destruction order control failed as expected")

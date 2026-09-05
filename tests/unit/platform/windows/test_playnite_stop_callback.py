#!/usr/bin/env python3
"""Exercise the production queued Playnite stop callback with lifecycle races.

Compile just the platform-independent callback against process/task seams so the
Windows handler's session identity and lock handoff can be checked on any host.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("repo", nargs="?", type=Path, default=Path(__file__).resolve().parents[4])
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()
text = (args.repo / "src/platform/windows/playnite_integration.cpp").read_text()
start = text.index("task_pool.push([expected_guard = std::move(guard)]() {")
end = text.index("          });", start) + len("          });")
callback = text[start:end]
prefix = r'''
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
struct Guard {
  bool has_active_app = true, uses_playnite = true;
  std::string playnite_id = "game", client_uuid = "client";
  int launch_started_at = 100;
};
std::ostringstream messages;
#define BOOST_LOG(level) messages
std::mutex lifecycle;
namespace nvhttp { std::mutex &stream_lifecycle_mutex() { return lifecycle; } }
struct Pool {
  std::vector<std::function<void()>> tasks;
  void push(std::function<void()> f) { tasks.push_back(std::move(f)); }
} task_pool;
bool inject_launch = false;
std::promise<void> snapshot_taken, launch_attempted;
auto launch_attempted_future = launch_attempted.get_future();
std::thread::id pipe_worker_id;
std::atomic<bool> terminated_from_pipe{false};
int terminations = 0;
std::string terminated_game;
namespace proc {
struct Process {
  Guard current;
  Guard active_session_guard() {
    auto snapshot = current;
    if (inject_launch) {
      snapshot_taken.set_value();
      launch_attempted_future.wait();
    }
    return snapshot;
  }
  void terminate(bool skip_display_revert = false, bool lock_held = false) {
    // This is the same lock ownership contract as proc_t::terminate.
    std::unique_lock<std::mutex> lock;
    if (!lock_held) lock = std::unique_lock<std::mutex>{lifecycle};
    if (skip_display_revert) throw std::runtime_error("changed display-revert policy");
    terminated_from_pipe = std::this_thread::get_id() == pipe_worker_id;
    ++terminations;
    terminated_game = current.playnite_id;
    current.has_active_app = false;
  }
  void terminate(bool immediate, bool needs_refresh, bool skip_display_revert, bool lock_held) {
    if (immediate || !needs_refresh) throw std::runtime_error("changed downstream termination policy");
    terminate(skip_display_revert, lock_held);
  }
} proc;
}
void dispatch(Guard guard) {
'''
suffix = r'''
}
int main(int argc, char **argv) {
  const int scenario = std::stoi(argv[1]);
  Guard expected;
  std::thread pipe([&] { pipe_worker_id = std::this_thread::get_id(); dispatch(expected); });
  pipe.join();
  if (task_pool.tasks.size() != 1 || terminations) return 10;
  if (scenario == 1) proc::proc.current.has_active_app = false;
  if (scenario == 2) proc::proc.current.uses_playnite = false;
  if (scenario == 3) proc::proc.current.playnite_id = "new-game";
  if (scenario == 4) proc::proc.current.client_uuid = "new-client";
  if (scenario == 5) proc::proc.current.launch_started_at = 101;
  bool launch_won = false;
  std::thread launcher;
  if (scenario == 6) {
    inject_launch = true;
    auto ready = snapshot_taken.get_future();
    launcher = std::thread([ready = std::move(ready), &launch_won]() mutable {
      ready.wait();
      // At the precise guard/termination boundary, a launch needs this lock.
      std::unique_lock<std::mutex> lock{lifecycle, std::try_to_lock};
      launch_won = lock.owns_lock();
      if (launch_won) proc::proc.current.playnite_id = "new-game";
      launch_attempted.set_value();
    });
  }
  for (auto &task : task_pool.tasks) task();
  if (launcher.joinable()) launcher.join();
  const bool should_terminate = scenario == 0 || scenario == 6;
  bool ok = terminations == (should_terminate ? 1 : 0) && !terminated_from_pipe;
  if (scenario == 6) ok = ok && !launch_won && terminated_game == "game";
  std::cout << "scenario=" << scenario << " terminations=" << terminations
            << " launch_won=" << launch_won << " terminated_game=" << terminated_game
            << " result=" << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
'''
with tempfile.TemporaryDirectory(prefix="playnite-stop-") as directory:
    directory = Path(directory)
    source = directory / "test.cpp"
    executable = directory / "test"
    source.write_text(prefix + callback + suffix)
    subprocess.run([args.compiler, "-std=c++20", "-pthread", str(source), "-o", str(executable)], check=True)
    for scenario in range(7):
        subprocess.run([str(executable), str(scenario)], check=True, timeout=5)

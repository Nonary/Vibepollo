#!/usr/bin/env python3
"""Run the production watchdog start/stop and missed-REVERT replay on real threads.

start_watchdog(), the stop functions, the watchdog globals and
retry_failed_revert_if_idle() are compiled from display_helper_integration.cpp.
Helper IPC, the lifecycle gate and host state are stubs. A fake worker runs the
production replay once per tick the test requests, so each ordering is exact.

usage: test_display_helper_failed_revert_watchdog.py [repo] [--compiler c++]
       [--source-file display_helper_integration.cpp]
"""

import argparse
import pathlib
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start + len(signature) - 1)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


PROGRAM = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "src/platform/windows/failed_revert_policy.h"

struct null_log_t {
  template<class T>
  null_log_t &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) null_log_t {}

namespace nvhttp {
  std::mutex &stream_lifecycle_mutex() {
    static std::mutex mutex;
    return mutex;
  }
}

namespace rtsp_stream {
  bool has_pending_launch_or_startup() { return false; }
  int session_count_no_cleanup();
}

namespace proc {
  struct proc_t {
    int current_app_id() { return 0; }
  } proc;
}

namespace display_helper_integration {
  struct RecoveryTicket {};
  enum class RevertOwnershipPolicy {
    AllOwnersMustRelease,
    RetainPausedNormalGame,
  };
  enum class FinalRestoreDisplayPolicy {
    Retain,
    RemoveAfterRestore,
  };
  bool revert(bool, bool, bool, RecoveryTicket *, RevertOwnershipPolicy, FinalRestoreDisplayPolicy);
  void start_watchdog();
  void stop_watchdog(bool force = false);
  bool stop_watchdog_unless_failed_revert();
}

namespace {
  display_helper::failed_revert_policy::slot_t g_failed_revert;

  bool stream_is_active_or_pending() { return false; }
  std::optional<std::uint64_t> active_session_generation() { return std::nullopt; }

@GLOBALS@

  bool reset_connection_and_clear_active_session_if_generation(const std::optional<std::uint64_t> &) {
    return true;
  }

@FINISH_STOP@

@RETRY@

  // The test's worker: one production replay per requested tick.
  std::mutex tick_mutex;
  std::condition_variable_any tick_cv;
  int ticks_requested = 0;
  int ticks_done = 0;
  std::atomic<int> workers_started = 0;

  void watchdog_proc(std::stop_token stop) {
    ++workers_started;
    std::unique_lock lock(tick_mutex);
    while (tick_cv.wait(lock, stop, [] { return ticks_requested > ticks_done; })) {
      lock.unlock();
      retry_failed_revert_if_idle();
      lock.lock();
      ++ticks_done;
      tick_cv.notify_all();
    }
  }
}

namespace display_helper_integration {
@START@

@STOP@
}

namespace {
  using namespace std::chrono_literals;
  using policy_request_t = display_helper::failed_revert_policy::request_t;

  int failures = 0;
  int reverts = 0;
  bool last_allow_disabled_recovery = false;
  // Runs on the worker after a replayed REVERT succeeds and before the worker
  // decides whether to stop.
  std::function<void()> after_replay;

  void check(bool condition, const char *name) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << name << '\n';
    failures += condition ? 0 : 1;
  }

  bool tick() {
    std::unique_lock lock(tick_mutex);
    const int target = ++ticks_requested;
    tick_cv.notify_all();
    if (tick_cv.wait_for(lock, 5s, [&] { return ticks_done >= target; })) {
      return true;
    }
    --ticks_requested;
    return false;
  }

  bool watchdog_running() {
    std::lock_guard lock(g_watchdog_mutex);
    return g_watchdog_state == watchdog_state_e::running;
  }

  void reset() {
    display_helper_integration::stop_watchdog(true);
    g_failed_revert.clear();
    reverts = 0;
    last_allow_disabled_recovery = false;
    after_replay = nullptr;
    std::lock_guard lock(tick_mutex);
    ticks_requested = ticks_done = 0;
    workers_started = 0;
  }

  policy_request_t missed_revert(bool allow_disabled_recovery) {
    policy_request_t request;
    request.allow_disabled_recovery = allow_disabled_recovery;
    return request;
  }

  // What revert() does when the helper is unreachable: remember the REVERT,
  // then start the watchdog once its locks are released.
  void miss_revert_on_another_thread(bool allow_disabled_recovery) {
    std::thread([&] {
      g_failed_revert.remember(missed_revert(allow_disabled_recovery));
      display_helper_integration::start_watchdog();
    }).join();
  }

  void revert_missed_while_the_worker_finishes_a_replay() {
    reset();
    g_failed_revert.remember(missed_revert(false));
    display_helper_integration::start_watchdog();
    after_replay = [] {
      miss_revert_on_another_thread(true);
    };
    const bool first_replayed = tick() && reverts == 1;
    const bool kept = watchdog_running() && g_failed_revert.pending();
    const bool second_replayed = tick() && reverts == 2 && last_allow_disabled_recovery;
    check(first_replayed && kept && second_replayed && !g_failed_revert.pending() && workers_started == 1,
          "a REVERT missed while the worker finishes a replay keeps the worker until it replays");
  }

  void revert_missed_after_the_worker_stopped_itself() {
    reset();
    g_failed_revert.remember(missed_revert(false));
    display_helper_integration::start_watchdog();
    const bool first_replayed = tick() && reverts == 1;
    const bool stopping = !watchdog_running();
    miss_revert_on_another_thread(true);
    const bool restarted = watchdog_running();
    // The tick also waits for the new worker thread to begin.
    const bool second_replayed = tick() && reverts == 2 && last_allow_disabled_recovery && workers_started == 2;
    check(first_replayed && stopping && restarted && second_replayed && !g_failed_revert.pending(),
          "a REVERT missed after the worker stopped itself starts a new worker that replays it");
  }

  void stream_end_keeps_the_watchdog_for_a_missed_revert() {
    reset();
    display_helper_integration::start_watchdog();
    g_failed_revert.remember(missed_revert(false));
    const bool kept = display_helper_integration::stop_watchdog_unless_failed_revert() && watchdog_running();
    g_failed_revert.clear();
    const bool stopped = !display_helper_integration::stop_watchdog_unless_failed_revert() && !watchdog_running();
    check(kept && stopped, "the end-of-stream stop keeps the worker only while a missed REVERT waits");
  }
}

int rtsp_stream::session_count_no_cleanup() {
  if (const auto hook = std::exchange(after_replay, nullptr)) {
    hook();
  }
  return 0;
}

bool display_helper_integration::revert(
  bool,
  bool,
  bool allow_disabled_recovery,
  RecoveryTicket *,
  RevertOwnershipPolicy,
  FinalRestoreDisplayPolicy
) {
  ++reverts;
  last_allow_disabled_recovery = allow_disabled_recovery;
  return true;
}

int main() {
  revert_missed_while_the_worker_finishes_a_replay();
  revert_missed_after_the_worker_stopped_itself();
  stream_end_keeps_the_watchdog_for_a_missed_revert();
  display_helper_integration::stop_watchdog(true);
  std::cout << "Failed REVERT watchdog failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", nargs="?", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--source-file", type=pathlib.Path)
    args = parser.parse_args()
    path = args.source_file or args.repository / "src/platform/windows/display_helper_integration.cpp"
    source = path.read_text()

    globals_block = section(source, "  enum class watchdog_state_e {",
                            "  static std::chrono::steady_clock::time_point g_last_vd_reenable")
    stop = function(source, "  static bool stop_watchdog_impl(const bool force, const bool keep_for_failed_revert) {")
    stop += "\n" + function(source, "  void stop_watchdog(bool force) {")
    stop += "\n" + function(source, "  bool stop_watchdog_unless_failed_revert() {")
    program = (PROGRAM
               .replace("@GLOBALS@", globals_block)
               .replace("@FINISH_STOP@", function(
                   source, "  static void finish_watchdog_stop_locked(const std::optional<std::uint64_t> &expected_generation) {"))
               .replace("@RETRY@", function(source, "  static void retry_failed_revert_if_idle() {"))
               .replace("@START@", function(source, "  void start_watchdog() {"))
               .replace("@STOP@", stop))

    with tempfile.TemporaryDirectory(prefix="failed-revert-watchdog-") as temporary:
        directory = pathlib.Path(temporary)
        cpp = directory / "test.cpp"
        binary = directory / "failed-revert-watchdog"
        cpp.write_text(program)
        subprocess.run(
            [args.compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-I", str(args.repository), str(cpp), "-o", str(binary)],
            check=True, timeout=120,
        )
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()

"""Exercise main's production PyroWave shutdown guard under its owned deadline."""
import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/main.cpp").read_text()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise AssertionError(f"expected exactly one production seam: {old!r}")
    return text.replace(old, new, 1)


deadline_start = source.index("  class shutdown_deadline_t {")
deadline_end = source.index("\n  };", deadline_start) + len("\n  };")
deadline = source[deadline_start:deadline_end]
# Keep the real worker, arming, synchronization, completion and forced-exit
# branches. Shorten only the wait durations for the deliberately blocked child.
deadline = replace_once(deadline, "std::chrono::seconds(15)", "std::chrono::milliseconds(150)")
deadline = replace_once(deadline, "std::chrono::seconds(10)", "std::chrono::milliseconds(100)")
deadline = replace_once(
    deadline,
    "~shutdown_deadline_t() {\n      complete();\n    }",
    '~shutdown_deadline_t() {\n      complete();\n      record("deadline completed");\n    }',
)

declaration = "  shutdown_deadline_t shutdown_deadline {&shutdown_signal_requested, supervised_machine_host, shutdown_phase_state};"
deadline_at = source.index(declaration)
guard_at = source.index("  auto pyrowave_shutdown_guard = util::fail_guard(")
guard_end = source.index("\n  });", guard_at) + len("\n  });")
shutdown_guard = source[guard_at:guard_end]
if deadline_at >= guard_at:
    raise AssertionError("PyroWave shutdown guard must outlive all later startup owners")
for marker in (
    "  std::thread termination_signal_monitor;",
    "  auto platf_deinit_guard = platf::init();",
    "video::probe_encoders()",
    "  std::thread configThread {confighttp::start};",
    "  std::thread rtspThread {rtsp_stream::start};",
):
    if guard_end >= source.index(marker, deadline_at):
        raise AssertionError(f"PyroWave shutdown guard must precede {marker.strip()}")

probe_at = source.index("  startup_probe();", guard_end)
probe_end = source.index("\n  }", probe_at) + len("\n  }")
probe_return = source[probe_at:probe_end]
if "return lifetime::desired_exit_code;" not in probe_return:
    raise AssertionError("fixture no longer contains main's early return after encoder probing")
shutdown_at = source.index("  // Wait for shutdown", probe_end)
shutdown_end = source.index("  rtspThread.join();", shutdown_at) + len("  rtspThread.join();")
normal_shutdown = source[shutdown_at:shutdown_end]

program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include "src/utility.h"
#include "src/shutdown_phase.h"
using namespace std::literals;

std::mutex records_mutex;
std::vector<std::string> records;
void record(const char *message) {
  std::lock_guard lock(records_mutex);
  records.emplace_back(message);
  std::puts(message);
  std::fflush(stdout);
}
struct NullLog {
  template<class T> NullLog &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) NullLog {}
namespace logging { void log_flush() { record("deadline expired"); } }
namespace lifetime {
  constexpr int desired_exit_code = 73;
  void debug_trap() { std::_Exit(74); }
}

PRODUCTION_DEADLINE

bool block_vendor_release = false;
bool streams_drained = false;
namespace pyrowave::host {
  void shutdown() {
    assert(streams_drained);
    record("pyrowave shutdown");
    if (block_vendor_release) {
      std::mutex mutex;
      std::condition_variable never_released;
      std::unique_lock lock(mutex);
      never_released.wait(lock, [] { return false; });
    }
    record("pyrowave released");
  }
}
namespace platf::linux_private_display {
  void request_process_shutdown_preserve() { record("display preserve"); }
}
void stop_termination_signal_monitor() { record("signal monitor stopped"); }
struct event_t {
  bool requested = false;
  bool peek() const { return requested; }
  void view() { record("shutdown observed"); }
};
struct thread_t {
  const char *name;
  void join() { record(name); }
};

int run(bool stop_after_probe) {
  auto shutdown_event = std::make_shared<event_t>();
  std::atomic_bool shutdown_signal_requested {false};
  const bool supervised_machine_host = true;
  shutdown_phase::tracker_t shutdown_phase_state;
PRODUCTION_DECLARATION
PRODUCTION_SHUTDOWN_GUARD
  auto termination_signal_monitor_guard = util::fail_guard([] {
    record("signal monitor guard");
  });
  // Later scope owners stand in for stream/device resources. They must finish
  // before main's extracted guard releases the final shared instance owner.
  auto stream_guard = util::fail_guard([] {
    streams_drained = true;
    record("streams drained");
  });
  thread_t configThread {"config joined"};
  thread_t httpThread {"http joined"};
  thread_t rtspThread {"rtsp joined"};
  auto startup_probe = [&] {
    record("encoder probe");
    shutdown_event->requested = stop_after_probe;
  };
PRODUCTION_PROBE_RETURN
PRODUCTION_NORMAL_SHUTDOWN
  return lifetime::desired_exit_code;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const bool early = !std::strcmp(argv[1], "early") ||
                     !std::strcmp(argv[1], "blocked-early");
  block_vendor_release = !std::strcmp(argv[1], "blocked") ||
                         !std::strcmp(argv[1], "blocked-early");
  assert(run(early) == lifetime::desired_exit_code);
  // Both normal and probe-return paths must return here after the real
  // deadline worker has completed and joined, with no static driver release.
  assert(!block_vendor_release);
  const std::vector<std::string> tail {
    "pyrowave shutdown", "pyrowave released", "deadline completed"
  };
  assert(records.size() >= tail.size());
  assert(std::equal(tail.begin(), tail.end(), records.end() - tail.size()));
  record("main returned");
}
'''
for placeholder, body in (
    ("PRODUCTION_DEADLINE", deadline),
    ("PRODUCTION_DECLARATION", declaration),
    ("PRODUCTION_SHUTDOWN_GUARD", shutdown_guard),
    ("PRODUCTION_PROBE_RETURN", probe_return),
    ("PRODUCTION_NORMAL_SHUTDOWN", normal_shutdown),
):
    program = replace_once(program, placeholder, body)

with tempfile.TemporaryDirectory(prefix="pyrowave-shutdown-") as temporary:
    path = pathlib.Path(temporary)
    cpp, executable = path / "test.cpp", path / "test"
    cpp.write_text(program)
    subprocess.run(
        [compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-I", str(root), *sys.argv[3:], str(cpp), "-o", str(executable)],
        check=True,
    )
    for mode in ("normal", "early", "blocked", "blocked-early"):
        result = subprocess.run([str(executable), mode], capture_output=True,
                                text=True, timeout=5)
        expected = 73 if mode.startswith("blocked") else 0
        if result.returncode != expected:
            raise AssertionError(
                f"{mode}: exit {result.returncode}, expected {expected}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        events = result.stdout.splitlines()
        if events.index("streams drained") >= events.index("pyrowave shutdown"):
            raise AssertionError(f"{mode}: shared Vulkan instance released before stream resources")
        if mode.startswith("blocked"):
            # Vibepollo intentionally exits before flushing the asynchronous log
            # sink, so the source-only "deadline expired" marker is not observable.
            # The expected process exit code plus this terminal event prove that
            # the owned deadline fired while the vendor release remained blocked.
            if events[-1] != "pyrowave shutdown" or "deadline completed" in events:
                raise AssertionError(f"{mode}: blocked vendor release escaped the owned shutdown deadline")
        elif events[-3:] != ["pyrowave released", "deadline completed", "main returned"]:
            raise AssertionError(f"{mode}: finite vendor release did not complete before deadline teardown")
        if mode.endswith("early") and "shutdown observed" in events:
            raise AssertionError(f"{mode}: fixture did not take the production encoder-probe return")

print("Production PyroWave shutdown order, probe-return cleanup and blocked-driver deadline passed")

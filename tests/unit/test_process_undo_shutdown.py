"""Exercise production undo-command teardown with deterministic process/time seams."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
negative_controls = "--negative-controls" in sys.argv[3:]
compiler_arguments = [arg for arg in sys.argv[3:] if arg != "--negative-controls"]
source = (root / "src/process.cpp").read_text()
start = source.index("    const auto undo_timeout =")
finish = source.index("\n    _pipe.reset();", start)
body = source[start:finish]
# Keep the production launch, wait, cancellation, detach and error branches.
# Substitute only time so a stuck command never makes this test wait 15 seconds.
body = body.replace("std::chrono::steady_clock", "fake_clock")
body = body.replace("std::this_thread::sleep_for", "fake_sleep_for")
main_source = (root / "src/main.cpp").read_text()
signal_start = main_source.index("  on_signal(SIGINT,")
signal_end = main_source.index("\n#endif", main_source.index("  on_signal(SIGTERM,", signal_start))
handlers = main_source[signal_start:signal_end]
signal_state = next(line for line in main_source.splitlines()
                    if line.startswith("  ") and "shutdown_signal_requested" in line
                    and ("std::atomic_bool shutdown_signal_requested" in line
                         or "auto &shutdown_signal_requested" in line))
entry_source = (root / "src/entry_handler.cpp").read_text()
exit_start = entry_source.index("  void exit_sunshine(")
exit_end = entry_source.index("\n  void debug_trap()", exit_start)
exit_request = entry_source[exit_start:exit_end]
program = r'''
#include "src/prep_command_lifetime.h"
#include "src/shutdown_signal.h"
#include "src/thread_safe.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
using namespace std::literals;
using milliseconds = std::chrono::milliseconds;
namespace command_lifetime = proc::command_lifetime;
struct fake_clock {
  static inline milliseconds elapsed {};
  static milliseconds now() { return elapsed; }
};
namespace mail {
  constexpr std::string_view shutdown = "shutdown";
  safe::mail_t man;
}
safe::mail_raw_t::event_t<bool> shutdown_signal;
milliseconds cancel_at;
milliseconds signal_cancel_at;
int sleeps;
void publish_signal_for_test(); // A second translation unit shares the flag.
void fake_sleep_for(milliseconds duration) {
  ++sleeps;
  fake_clock::elapsed += duration;
  if (mail::man && fake_clock::elapsed >= cancel_at) shutdown_signal->raise(true);
  if (fake_clock::elapsed >= signal_cancel_at) publish_signal_for_test();
}
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
namespace lifetime {
  std::atomic_int desired_exit_code {0};
PRODUCTION_EXIT_REQUEST
}
namespace boost::filesystem { using path = std::string; }
struct command_t {
  bool elevated = false;
  std::string undo_cmd;
  milliseconds duration = 500ms;
  bool spawn_error = false, wait_error = false, detach_error = false;
  int result = 0;
};
std::vector<command_t> commands;
std::vector<std::string> launched;
int waits, detaches;
struct child_t {
  const command_t &command;
  milliseconds started;
  bool running() const { return fake_clock::elapsed - started < command.duration; }
  void detach() {
    ++detaches;
    if (command.detach_error) throw 1;
  }
  void wait(std::error_code &ec) {
    assert(!running());
    ++waits;
    if (command.wait_error) ec = std::make_error_code(std::errc::io_error);
  }
  int exit_code() const { return command.result; }
};
namespace platf {
  child_t run_command(bool, bool, const std::string &name, const std::string &, int, void *, std::error_code &ec, void *) {
    const auto &command = *std::find_if(commands.begin(), commands.end(), [&](const auto &c) { return c.undo_cmd == name; });
    launched.push_back(name);
    if (command.spawn_error) ec = std::make_error_code(std::errc::permission_denied);
    return {command, fake_clock::elapsed};
  }
}
std::string find_working_directory(const std::string &, int) { return {}; }
void run_undo(std::chrono::seconds timeout = 0s) {
  struct { std::chrono::seconds exit_timeout; std::string working_dir; } _app {timeout, {}};
  auto _app_prep_begin = commands.begin(), _app_prep_it = commands.end();
  int _env = 0;
  std::unique_ptr<int> _pipe;
  std::error_code ec;
PRODUCTION_UNDO
  assert(!ec);
}
void reset(bool shutting_down = false) {
  lifetime::shutdown_signal_requested.store(false);
  fake_clock::elapsed = 0ms;
  cancel_at = milliseconds::max();
  signal_cancel_at = milliseconds::max();
  mail::man = std::make_shared<safe::mail_raw_t>();
  shutdown_signal = mail::man->event<bool>(mail::shutdown);
  if (shutting_down) {
    shutdown_signal->raise(true);
    assert(shutdown_signal->view()); // main observes shutdown without consuming it.
  }
  commands = {{false, "undo"}};
  launched.clear();
  sleeps = waits = detaches = 0;
}
std::map<int, std::function<void()>> handlers;
template<class F> void on_signal(int sig, F callback) { handlers[sig] = callback; }
int terminations;
bool event_at_terminate;
namespace proc {
  struct process_t {
    void terminate() {
      ++terminations;
      event_at_terminate = shutdown_signal->peek();
      run_undo();
    }
  } proc;
}
int main() {
  // Exercise the actual SIGINT callback through the actual undo block. It must
  // cancel before publishing mail, preserving Apollo's eager cleanup owner.
  // SIGTERM publishes mail without eager termination and remains cancellable.
  for (int signal : {SIGINT, SIGTERM}) {
    reset();
    terminations = 0;
    commands[0].duration = 7200s;
    const auto shutdown_event = shutdown_signal;
PRODUCTION_SIGNAL_STATE
PRODUCTION_SIGNAL_HANDLERS
    handlers.at(signal)();
    assert(shutdown_signal->peek());
    assert(shutdown_signal_requested.load());
    if (signal == SIGINT) {
      assert(terminations == 1 && !event_at_terminate);
    } else {
      assert(terminations == 0);
      proc::proc.terminate(); // Later main/deinit cleanup.
      assert(event_at_terminate);
    }
    std::cout << (signal == SIGINT ? "SIGINT" : "SIGTERM")
              << ": undo_elapsed_ms=" << fake_clock::elapsed.count()
              << ", mail_at_terminate=" << event_at_terminate << std::endl;
    assert(detaches == 1 && waits == 0 && fake_clock::elapsed == 0ms);
  }

  // HTTP, tray, WM_ENDSESSION and console-close requests use exit_sunshine's
  // ordinary-thread mailbox path rather than the signal flag.
  reset();
  commands[0].duration = 7200s;
  lifetime::exit_sunshine(0, true);
  assert(shutdown_signal->peek() && !lifetime::shutdown_signal_requested.load());
  run_undo();
  assert(detaches == 1 && waits == 0 && fake_clock::elapsed == 0ms);

  // App stop still waits for successful undo completion and collects its status.
  reset();
  run_undo();
  assert(launched.size() == 1 && waits == 1 && detaches == 0);
  assert(fake_clock::elapsed == 500ms);

  // A restart still launches cleanup, but must not spend its 10-second watchdog
  // budget on even one command's 15-second minimum (or a longer app timeout).
  for (auto timeout : {0s, 3600s}) {
    reset(true);
    commands[0].duration = 7200s;
    run_undo(timeout);
    assert(launched.size() == 1 && waits == 0 && detaches == 1);
    assert(fake_clock::elapsed == 0ms);
  }

  // Shutdown arriving during a normal app stop interrupts the existing wait.
  reset();
  commands[0].duration = 30s;
  cancel_at = 200ms;
  run_undo();
  assert(waits == 0 && detaches == 1 && fake_clock::elapsed == 200ms);

  // A signal can interrupt an ordinary app stop before any mailbox publisher
  // runs, including when process mail is unavailable during startup teardown.
  for (bool no_mail : {false, true}) {
    reset();
    if (no_mail) mail::man.reset();
    commands[0].duration = 30s;
    signal_cancel_at = 200ms;
    run_undo();
    assert(!shutdown_signal->peek());
    assert(waits == 0 && detaches == 1 && fake_clock::elapsed == 200ms);
  }

  // All undo actions still launch in reverse order during shutdown. Empty and
  // failed actions, including detach failure, do not suppress later cleanup.
  reset(true);
  commands = {{false, "first", 30s}, {false, "", 30s}, {false, "failed", 30s, true},
              {false, "last", 30s, false, false, true}};
  run_undo();
  assert((launched == std::vector<std::string>{"last", "failed", "first"}));
  assert(waits == 0 && detaches == 2 && fake_clock::elapsed == 0ms);

  // Finished commands can still be reaped during shutdown.
  reset(true);
  commands[0].duration = 0ms;
  run_undo();
  assert(waits == 1 && detaches == 0 && sleeps == 0);

  // Ordinary timeout and no-mail fallback preserve the existing 15-second floor.
  for (bool no_mail : {false, true}) {
    reset();
    if (no_mail) mail::man.reset();
    commands[0].duration = 30s;
    run_undo();
    assert(detaches == 1 && waits == 0 && fake_clock::elapsed == 15s);
  }
  reset();
  commands[0].duration = 30s;
  run_undo(20s);
  assert(detaches == 1 && waits == 0 && fake_clock::elapsed == 20s);

  // Exit and wait errors continue to the remaining undo action.
  for (bool wait_error : {false, true}) {
    reset();
    commands = {{false, "first", 0ms}, {false, "last", 0ms, false, wait_error, false, 7}};
    run_undo();
    assert(launched.size() == 2 && waits == 2 && detaches == 0);
  }
}
'''.replace("PRODUCTION_UNDO", body).replace("PRODUCTION_SIGNAL_STATE", signal_state).replace(
    "PRODUCTION_SIGNAL_HANDLERS", handlers).replace("PRODUCTION_EXIT_REQUEST", exit_request)
controls = []
if negative_controls:
    # Each mutation must compile and fail its specific runtime invariant. Keep
    # these opt-in so ordinary CTest retains its single-compile cost.
    for name, before, after, assertion in (
        ("mailbox-only undo",
         "return lifetime::shutdown_signal_requested.load(std::memory_order_relaxed) ||\n"
         "             (shutdown_event && shutdown_event->peek());",
         "return shutdown_event && shutdown_event->peek();",
         "detaches == 1 && waits == 0 && fake_clock::elapsed == 0ms"),
        ("private main flag", signal_state,
         "  std::atomic_bool shutdown_signal_requested {false};",
         "detaches == 1 && waits == 0 && fake_clock::elapsed == 0ms"),
        ("early mailbox publication",
         "proc::proc.terminate();\n    // Break out of the main loop\n    shutdown_event->raise(true);",
         "shutdown_event->raise(true);\n    // Break out of the main loop\n    proc::proc.terminate();",
         "terminations == 1 && !event_at_terminate"),
        ("ordinary app-stop cancellation",
         "return lifetime::shutdown_signal_requested.load(std::memory_order_relaxed) ||",
         "return true || lifetime::shutdown_signal_requested.load(std::memory_order_relaxed) ||",
         "launched.size() == 1 && waits == 1 && detaches == 0"),
    ):
        assert program.count(before) == 1, name
        controls.append((name, program.replace(before, after), assertion))
with tempfile.TemporaryDirectory(prefix="undo-shutdown-") as temporary:
    path = pathlib.Path(temporary)
    cpp, exe = path / "test.cpp", path / "test"
    cpp.write_text(program)
    signal_cpp = path / "signal.cpp"
    signal_cpp.write_text('''#include "src/shutdown_signal.h"
void publish_signal_for_test() {
  lifetime::shutdown_signal_requested.store(true, std::memory_order_relaxed);
}
''')
    compile_command = [compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-pthread", "-I", str(root), *compiler_arguments,
                       str(cpp), str(signal_cpp), "-o", str(exe)]
    subprocess.run(compile_command, check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
    for name, mutant, assertion in controls:
        cpp.write_text(mutant)
        subprocess.run(compile_command, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and assertion in result.stderr, (name, result.stdout, result.stderr)
        print(f"Negative control rejected: {name}")
print("Production undo: SIGINT/SIGTERM ordering, signal/mail cancellation, ordinary timeout and error paths passed")

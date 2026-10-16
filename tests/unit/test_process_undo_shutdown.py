"""Exercise production undo-command teardown with deterministic process/time seams."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/process.cpp").read_text()
start = source.index("    const auto undo_timeout =")
finish = source.index("\n    _pipe.reset();", start)
body = source[start:finish]
# Keep the production launch, wait, cancellation, detach and error branches.
# Substitute only time so a stuck command never makes this test wait 15 seconds.
body = body.replace("std::chrono::steady_clock", "fake_clock")
body = body.replace("std::this_thread::sleep_for", "fake_sleep_for")
program = r'''
#include "src/thread_safe.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
using namespace std::literals;
using milliseconds = std::chrono::milliseconds;
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
int sleeps;
void fake_sleep_for(milliseconds duration) {
  ++sleeps;
  fake_clock::elapsed += duration;
  if (mail::man && fake_clock::elapsed >= cancel_at) shutdown_signal->raise(true);
}
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
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
  fake_clock::elapsed = 0ms;
  cancel_at = milliseconds::max();
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
int main() {
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
'''.replace("PRODUCTION_UNDO", body)
with tempfile.TemporaryDirectory(prefix="undo-shutdown-") as temporary:
    path = pathlib.Path(temporary)
    cpp, exe = path / "test.cpp", path / "test"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-I", str(root),
                    *sys.argv[3:], str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print("Production undo launch, shutdown cancellation, ordinary timeout and error paths passed")

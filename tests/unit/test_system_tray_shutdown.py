"""Exercise production Windows tray startup/stop paths without opening a UI."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/system_tray.cpp").read_text()
wait_start = source.index("    // Wait for the shell to be initialized")
run_start = source.index("  void run_tray() {")
end_finish = source.index("\n  void update_tray_playing", run_start)
startup = source[wait_start:run_start]
# Vibepollo keeps initialization separate for its compatibility entry point.
entry = "static int init_tray() {" if "  static int init_tray() {" in source else "int system_tray() {"
lifecycle = source[run_start:end_finish]
compatibility_start = source.index("  int init_tray_threaded() {")
compatibility_end = source.index("\n  void tray_notify", compatibility_start)
compatibility = source[compatibility_start:compatibility_end]
main_source = (root / "src/main.cpp").read_text()
guard_start = main_source.index("  auto tray_shutdown_guard = util::fail_guard(")
guard_finish = main_source.index("\n  });", guard_start) + len("\n  });")
shutdown_guard = main_source[guard_start:guard_finish]

program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "src/system_tray_lifecycle.h"
using namespace std::literals;
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
using HDESK = void *;
using WCHAR = wchar_t;
using DWORD = unsigned long;
constexpr int FALSE = 0, DESKTOP_READOBJECTS = 1, DESKTOP_ENUMERATE = 2, UOI_NAME = 2;
std::atomic<bool> shell_ready, desktop_ready, fail_init;
std::atomic<bool> block_init, allow_init;
std::atomic<int> shell_queries, desktop_queries, init_calls, loop_calls, exit_calls;
void *GetShellWindow() { ++shell_queries; return shell_ready ? reinterpret_cast<void *>(1) : nullptr; }
HDESK OpenInputDesktop(int, int, int) { ++desktop_queries; return reinterpret_cast<void *>(1); }
bool GetUserObjectInformationW(HDESK, int, WCHAR *name, unsigned long, DWORD *) {
  std::wcscpy(name, desktop_ready ? L"Default" : L"Winlogon"); return true;
}
int _wcsicmp(const WCHAR *a, const WCHAR *b) { return std::wcscmp(a, b); }
void CloseDesktop(HDESK) {}
namespace util {
  template<class F> struct guard_t { F fn; ~guard_t() { fn(); } };
  template<class F> auto fail_guard(F fn) { return guard_t<F>{fn}; }
}
struct tray { struct { const char *text = nullptr; } menu[2]; };
namespace config {
  struct { std::string sunshine_name = "fixture"; } nvhttp;
  struct { bool hide_tray_controls = false; } sunshine;
}
namespace confighttp { constexpr int PORT_HTTPS = 47990; }
namespace net { int map_port(int port) { return port; } }
std::string utf8ToAcp(std::string value) { return value; }
int tray_init(tray *) {
  ++init_calls;
  while (block_init && !allow_init) { std::this_thread::sleep_for(1ms); }
  return fail_init ? -1 : 0;
}
int tray_loop(int) { ++loop_calls; return 0; }
void tray_exit() { ++exit_calls; }
void tray_log_bridge() {}
void tray_set_log_callback(void (*)()) {}
namespace system_tray {
  static std::atomic<bool> tray_initialized = false;
  static std::mutex tray_action_mutex;
  static std::condition_variable tray_action_cv;
  static std::queue<std::function<void()>> tray_actions;
  static detail::worker_t tray_worker;
  static struct tray tray;
  static std::function<void()> action;
  static void clear_pending_quit_messages() {}
  static void run_pending_tray_actions() {
    if (action) { std::exchange(action, {})(); }
  }
PRODUCTION_STARTUP
PRODUCTION_LIFECYCLE
PRODUCTION_COMPATIBILITY
}
template<class F> void await(F ready) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!ready()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
}
void reset(bool shell, bool desktop, bool failing = false) {
  system_tray::end_tray();
  shell_ready = shell; desktop_ready = desktop; fail_init = failing;
  block_init = false; allow_init = false;
  shell_queries = desktop_queries = init_calls = loop_calls = exit_calls = 0;
  system_tray::action = {};
}
void stop_promptly() {
  const auto start = std::chrono::steady_clock::now();
  system_tray::end_tray();
  assert(std::chrono::steady_clock::now() - start < 500ms);
  assert(!system_tray::tray_initialized);
}
void startup_return() {
  struct { bool armed = false; void arm() { armed = true; } } shutdown_deadline;
  struct VerifyDeadline {
    decltype(shutdown_deadline) &deadline;
    ~VerifyDeadline() { assert(deadline.armed); }
  } verify {shutdown_deadline};
  system_tray::run_tray();
  PRODUCTION_SHUTDOWN_GUARD
  await([] { return shell_queries > 0; });
  // An early main return must cancel/join before watchdog/global destruction.
}
int main() {
  reset(false, false);
  startup_return();
  assert(system_tray::tray_worker.stop_requested() && !system_tray::tray_initialized);
  // The reported failure: Explorer never appears, then a stop is requested.
  reset(false, false);
  system_tray::run_tray();
  await([] { return shell_queries > 0; });
  stop_promptly();
  assert(init_calls == 0 && exit_calls == 0);

  // A locked desktop and failed Shell_NotifyIcon retries must also cancel.
  reset(true, false);
  system_tray::run_tray();
  await([] { return desktop_queries > 0; });
  stop_promptly();
  assert(init_calls == 0 && exit_calls == 0);
  reset(true, true, true);
  system_tray::run_tray();
  await([] { return init_calls > 0; });
  stop_promptly();
  assert(init_calls == 1 && exit_calls == 0);

  // Waiting for Explorer and the unlocked desktop still permits normal startup.
  reset(false, false);
  system_tray::run_tray();
  await([] { return shell_queries > 0; });
  shell_ready = true;
  await([] { return desktop_queries > 0; });
  desktop_ready = true;
  await([] { return loop_calls > 0; });
  stop_promptly();
  assert(init_calls == 1 && exit_calls == 1);

  // Cancellation racing a successful native initialization still cleans up once.
  reset(true, true);
  block_init = true;
  system_tray::run_tray();
  await([] { return init_calls > 0; });
  std::thread init_stopper([] { system_tray::end_tray(); });
  await([] { return system_tray::tray_worker.stop_requested(); });
  allow_init = true;
  init_stopper.join();
  assert(init_calls == 1 && exit_calls == 1 && !system_tray::tray_initialized);

  // Normal initialization, duplicate starts and restart after a joined stop.
  for (int i = 0; i < 2; ++i) {
    reset(true, true);
    system_tray::run_tray();
    await([] { return loop_calls > 0; });
    assert(system_tray::tray_initialized);
    system_tray::run_tray();
    stop_promptly();
    assert(init_calls == 1 && exit_calls == 1);
    system_tray::end_tray();
    assert(exit_calls == 1);
  }

  // Main and the signal thread may stop concurrently; one owns the join.
  for (int iteration = 0; iteration < 40; ++iteration) {
    reset(false, false);
    system_tray::run_tray();
    await([] { return shell_queries > 0; });
    std::atomic<bool> go = false;
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) {
      callers.emplace_back([&] { await([&] { return go.load(); }); system_tray::end_tray(); });
    }
    go = true;
    for (auto &caller : callers) { caller.join(); }
    assert(init_calls == 0 && !system_tray::tray_initialized);
  }

  // A callback can stop itself without detaching; an external caller joins.
  reset(true, true);
  std::atomic<bool> self_stopped = false;
  system_tray::action = [&] { system_tray::end_tray(); self_stopped = true; };
  system_tray::run_tray();
  await([&] { return self_stopped.load(); });
  assert(!system_tray::tray_worker.start([] {}));
  stop_promptly();
  assert(exit_calls == 1);

  // Self-stop must not deadlock against an external caller already joining.
  reset(true, true);
  std::atomic<bool> in_callback = false, release_callback = false;
  system_tray::action = [&] {
    in_callback = true;
    await([&] { return release_callback.load(); });
    system_tray::end_tray();
  };
  system_tray::run_tray();
  await([&] { return in_callback.load(); });
  std::thread stopper([] { system_tray::end_tray(); });
  await([] { return system_tray::tray_worker.stop_requested(); });
  release_callback = true;
  stopper.join();
  assert(exit_calls == 1 && !system_tray::tray_initialized);

  // Apollo's compatibility entry points share the same cancellation/join owner.
  reset(true, true);
  assert(system_tray::init_tray_threaded() == 0);
  assert(system_tray::init_tray_threaded() == 1);
  assert(std::string(system_tray::tray.menu[0].text) == "Open Apollo (fixture:47990)");
  system_tray::end_tray_threaded();
  system_tray::end_tray_threaded();
  assert(exit_calls == 1 && !system_tray::tray_initialized);

  reset(false, false);
  int startup_status = -1;
  std::thread compatibility_startup([&] { startup_status = system_tray::init_tray_threaded(); });
  await([] { return shell_queries > 0; });
  system_tray::end_tray_threaded();
  compatibility_startup.join();
  assert(startup_status == 1 && init_calls == 0);

  reset(false, false);
  const auto timeout_start = std::chrono::steady_clock::now();
  assert(system_tray::init_tray_threaded() == 1);
  const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_start;
  assert(timeout_elapsed >= 10s && timeout_elapsed < 12s);
  assert(init_calls == 0 && !system_tray::tray_initialized);
}
'''.replace("PRODUCTION_STARTUP", entry + "\n#ifdef _WIN32\n" + startup).replace("PRODUCTION_LIFECYCLE", lifecycle).replace("PRODUCTION_SHUTDOWN_GUARD", shutdown_guard).replace("PRODUCTION_COMPATIBILITY", compatibility)

with tempfile.TemporaryDirectory(prefix="tray-shutdown-") as temporary:
    path = pathlib.Path(temporary)
    cpp, exe = path / "test.cpp", path / "test"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-D_WIN32", "-I", str(root), *sys.argv[3:], str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=20)
print("Production Windows tray startup, cancellation, restart and concurrent/self teardown passed")

"""Exercise main's NVIDIA cleanup ordering with deterministic API boundaries.

Compile the production initialization/cleanup blocks and actual early-return
branches with the real FailGuard. NVAPI and logging lifetimes are fixtures;
this does not qualify the Windows runtime or reproduce the Boost exception.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/main.cpp").read_text()
utility = (root / "src/utility.h").read_text()
guard_start = utility.index("  template<class T>\n  class FailGuard")
guard_end = utility.index("\n  template<class T>\n  void append_struct", guard_start)
guard = utility[guard_start:guard_end]

modify = source.index("  // Modify relevant NVIDIA control panel settings")
init_end = source.index("  // Wait as long as possible", modify)
initialization = source[modify:init_end]
callback_start = source.index("  auto restore_nvprefs =")
callback_end = source.index("\n", source.index("  auto nvprefs_shutdown_guard", callback_start))
callback = source[callback_start:callback_end]
assert source.index("auto log_deinit_guard") < callback_start < source.index("  if (!config::sunshine.cmd.name.empty())")
dispatch_start = source.index("  if (!config::sunshine.cmd.name.empty())")
dispatch_end = source.index("\n  // Display configuration", dispatch_start)
dispatch = source[dispatch_start:dispatch_end]
cleanup_start = source.index("  // Restore global NVIDIA control panel settings")
cleanup_end = source.index("#endif", cleanup_start)
cleanup = source[cleanup_start:cleanup_end]

# Use the real return branches rather than a second implementation of them.
branches = re.findall(
    r"  if \(shutdown_event->peek\(\)\) \{\n(?:    [^\n]*\n)*?  \}",
    source[init_end:cleanup_start],
)
assert len(branches) == 4, "Reassess cleanup coverage when startup exits change"
http_start = source.index("  if (http::init()) {")
http_end = source.index("\n  }", http_start) + len("\n  }")
branches.append(source[http_start:http_end])
returns = "\n".join(
    f"  if (selected == {i}) {{\n{branch}\n  }}"
    for i, branch in enumerate(branches, start=1)
)

program = r'''
#include <cassert>
#include <stdexcept>
#include <string>
#include <map>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>
using namespace std::literals;
namespace util {
PRODUCTION_GUARD
}
bool logging_alive = false;
std::vector<int> events;
struct Logger {
  Logger() { assert(!logging_alive); logging_alive = true; events.push_back(1); }
  ~Logger() { events.push_back(9); logging_alive = false; }
};
struct Nvprefs {
  bool fail_init = false;
  int shutdowns = 0;
  bool load() { return true; }
  void unload() {}
  void restore_from_and_delete_undo_file_if_exists() {}
  void modify_application_profile() {}
  void modify_global_profile() {
    if (fail_init) { throw std::runtime_error("fixture initialization failure"); }
  }
  bool shutdown() noexcept {
    assert(logging_alive);
    ++shutdowns;
    events.push_back(3);
    return true;
  }
} nvprefs_instance;
struct ShutdownEvent { bool peek() { return true; } } shutdown;
auto shutdown_event = &shutdown;
struct Thread { void join() { events.push_back(2); } } configThread;
namespace lifetime { constexpr int desired_exit_code = 17; }
namespace http { int init() { return 1; } }
struct NullLog {
  template<class T> NullLog &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) NullLog{}
namespace config { struct { struct { std::string name; int argc = 0; char **argv = nullptr; } cmd; } sunshine; }
std::map<std::string_view, std::function<int(const char *, int, char **)>> cmd_to_func {
  {"version", [](const char *, int, char **) { return 23; }}
};
int host_main(int selected) {
  char *argv[] = {nullptr};
  Logger log_deinit_guard;
PRODUCTION_CALLBACK
PRODUCTION_DISPATCH
PRODUCTION_INITIALIZATION
PRODUCTION_RETURNS
  events.push_back(4); // The existing normal shutdown cleanup position.
PRODUCTION_CLEANUP
  events.push_back(5);
  return lifetime::desired_exit_code;
}
void check(int selected, bool fail_init = false) {
  nvprefs_instance = {};
  nvprefs_instance.fail_init = fail_init;
  config::sunshine.cmd.name = selected == 6 ? "missing" : selected == 7 ? "version" : "";
  events.clear();
  bool threw = false;
  try {
    const int result = host_main(selected);
    assert(result == (selected == 5 ? -1 : selected == 6 ? 7 : selected == 7 ? 23 : lifetime::desired_exit_code));
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw == fail_init);
  assert(nvprefs_instance.shutdowns == 1);
  assert(!logging_alive && events.front() == 1 && events.back() == 9);
  if (!threw) {
    if (selected == 0) {
      assert((events == std::vector<int>{1, 4, 3, 5, 9}));
    } else if (selected == 3 || selected == 4) {
      assert((events == std::vector<int>{1, 2, 3, 9}));
    } else {
      assert((events == std::vector<int>{1, 3, 9}));
    }
  }
}
int main() {
  for (int exit = 0; exit <= 7; ++exit) { check(exit); }
  check(0, true);
}
'''
for marker, value in (
    ("PRODUCTION_GUARD", guard),
    ("PRODUCTION_CALLBACK", callback),
    ("PRODUCTION_DISPATCH", dispatch),
    ("PRODUCTION_INITIALIZATION", initialization),
    ("PRODUCTION_RETURNS", returns),
    ("PRODUCTION_CLEANUP", cleanup),
):
    program = program.replace(marker, value)

with tempfile.TemporaryDirectory(prefix="nvprefs-shutdown-") as directory:
    path = pathlib.Path(directory)
    (path / "test.cpp").write_text(program)
    executable = path / "test"
    subprocess.run(
        [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", str(path / "test.cpp"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
print("NVIDIA cleanup ordering: normal exit, five startup exits, two maintenance exits and initialization unwind passed")

#!/usr/bin/env python3
"""Exercise the production readiness function with deterministic time/outputs.

Usage: python3 test_virtual_capture_readiness.py [repository] [video.cpp]
The optional source path permits running the same regressions against an older
revision. Windows display enumeration and the monotonic clock are boundaries;
the readiness decision, timeout, state reset, and index selection are unmodified.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
source = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "src/video.cpp"
match = re.search(r"^    bool ensure_virtual_display_ready\([\s\S]*?^    }", source.read_text(), re.M)
if not match:
    raise RuntimeError("Missing production readiness function")
function = match.group().replace("std::chrono::steady_clock", "TestClock")
shutdown_match = re.search(
    r"      std::erase_if\(synced_session_ctxs,[\s\S]*?      if \(synced_session_ctxs.empty\(\)\) \{[\s\S]*?^      }",
    source.read_text(), re.M,
)
# Old revisions have no readiness-loop shutdown handling: tests still exercise
# their absence by leaving the wrapper body empty.
shutdown = shutdown_match.group() if shutdown_match else ""
harness = r'''
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
struct TestClock {
  using time_point = std::chrono::steady_clock::time_point;
  static inline time_point current{std::chrono::seconds(1)};
  static time_point now() { return current; }
};
std::ostringstream logs;
#define BOOST_LOG(level) logs
bool prefer_virtual = true;
std::optional<std::string> virtual_output = "virtual";
bool should_prefer_virtual_display() { return prefer_virtual; }
auto active_virtual_display_dxgi_name() { return virtual_output; }
namespace config { std::string get_active_output_name() { return "virtual"; } }
namespace boost {
  bool iequals(const std::string &a, const std::string &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
  }
}
// PRODUCTION
struct Event {
  bool value = false;
  bool peek() const { return value; }
  void raise(bool next) { value = next; }
};
struct Session { Event *shutdown_event; Event *join_event; };
enum class encode_e { ok, reinit };
encode_e check_readiness_shutdown(std::vector<std::unique_ptr<Session>> &synced_session_ctxs) {
// SHUTDOWN
  return encode_e::reinit;
}
int failures = 0;
void check(bool result, const char *name) {
  if (!result) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}
void advance(int seconds) { TestClock::current += std::chrono::seconds(seconds); }
void reset() {
  std::vector<std::string> empty;
  int index = 0;
  ensure_virtual_display_ready(empty, index);
  prefer_virtual = true;
  virtual_output = "virtual";
  advance(100);
}
int main() {
  int index = 0;
  std::vector<std::string> names{"physical"};
#ifdef _WIN32
  reset();
  check(!ensure_virtual_display_ready(names, index), "missing named output waits");
  for (int pass = 0; pass < 4; ++pass) {
    advance(4);
    check(!ensure_virtual_display_ready(names, index), "timeout cannot approve physical capture");
  }
  names.push_back("VIRTUAL");
  check(ensure_virtual_display_ready(names, index) && index == 1, "returning output is selected case-insensitively");

  reset(); names = {"physical"}; index = 0; virtual_output.reset();
  check(!ensure_virtual_display_ready(names, index), "unresolved identity waits");
  advance(4);
  check(!ensure_virtual_display_ready(names, index), "unresolved timeout cannot approve physical capture");

  // Preference changes (including lock-screen policy) release a pending wait.
  prefer_virtual = false;
  check(ensure_virtual_display_ready(names, index) && index == 0, "physical preference ends wait");
  prefer_virtual = true;
  check(!ensure_virtual_display_ready(names, index), "new virtual preference starts a fresh wait");

  // Topology-owned exact-output paths have already validated their own target.
  check(ensure_virtual_display_ready(names, index, false), "exact-output path bypasses process preference");
  check(!ensure_virtual_display_ready(names, index), "exact-output bypass clears stale wait");

  reset(); names.clear(); index = 5;
  check(!ensure_virtual_display_ready(names, index) && index == 0, "empty list remains unavailable");
  names = {"physical", "virtual"}; index = -1;
  check(ensure_virtual_display_ready(names, index) && index == 1, "ready target overrides invalid prior index");

  reset(); names = {"physical"}; index = 0;
  check(!ensure_virtual_display_ready(names, index), "disconnect begins a fresh wait");
  advance(4);
  check(!ensure_virtual_display_ready(names, index), "disconnect cannot select remaining physical output");
#else
  index = 5;
  check(ensure_virtual_display_ready(names, index) && index == 0, "non-Windows clamps index");
  names.clear();
  check(!ensure_virtual_display_ready(names, index), "non-Windows rejects empty list");
#endif
  Event stopped{true}, active, stopped_join, active_join;
  std::vector<std::unique_ptr<Session>> sessions;
  sessions.push_back(std::make_unique<Session>(&stopped, &stopped_join));
  sessions.push_back(std::make_unique<Session>(&active, &active_join));
  check(check_readiness_shutdown(sessions) == encode_e::reinit &&
        sessions.size() == 1 && stopped_join.value && !active_join.value,
        "cancelled session is joined while active peer keeps waiting");
  active.raise(true);
  check(check_readiness_shutdown(sessions) == encode_e::ok &&
        sessions.empty() && active_join.value,
        "last session cancellation ends readiness retry");
  return failures ? 1 : 0;
}
'''.replace("// PRODUCTION", function).replace("// SHUTDOWN", shutdown)
with tempfile.TemporaryDirectory(prefix="virtual-capture-readiness-") as temporary:
    cpp = Path(temporary) / "test.cpp"
    cpp.write_text(harness)
    for platform, flags in [("Windows", ["-D_WIN32"]), ("non-Windows", [])]:
        exe = Path(temporary) / platform
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", *flags, str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(f"{platform} production virtual capture readiness regressions passed")

#!/usr/bin/env python3
"""Exercise production Mouse Keys state ownership with a fake Windows desktop.

No Win32 host or repository build is required. --source accepts an older
misc.cpp to demonstrate the repeated-frame restoration regression.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::literals;
struct log_sink {
  template<class T> log_sink &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) log_sink{}
struct MOUSEKEYS {
  unsigned cbSize, dwFlags, iMaxSpeed, iTimeToMaxSpeed, iCtrlSpeed,
    dwReserved1, dwReserved2;
  bool operator==(const MOUSEKEYS &) const = default;
};
constexpr unsigned SM_MOUSEPRESENT = 1, SPI_GETMOUSEKEYS = 2,
  SPI_SETMOUSEKEYS = 3, MKF_MOUSEKEYSON = 1, MKF_AVAILABLE = 2;
std::mutex desktop_mutex;
MOUSEKEYS desktop;
bool mouse_present = false, fail_get = false, fail_set = false;
unsigned gets = 0, sets = 0;
int GetSystemMetrics(unsigned) { return mouse_present; }
int GetLastError() { return 5; }
int SystemParametersInfoW(unsigned action, unsigned, void *data, unsigned) {
  std::lock_guard lock(desktop_mutex);
  auto state = static_cast<MOUSEKEYS *>(data);
  if (action == SPI_GETMOUSEKEYS) {
    ++gets;
    if (fail_get) return 0;
    *state = desktop;
  } else {
    ++sets;
    if (fail_set) return 0;
    desktop = *state;
  }
  return 1;
}
'''

TESTS = r'''
unsigned failures = 0, cases = 0;
void check(bool ok, const char *name) {
  ++cases;
  if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
const MOUSEKEYS original {sizeof(MOUSEKEYS), 0x40, 77, 3000, 5, 0, 0};
void reset(MOUSEKEYS initial = original) {
  desktop = initial;
  gets = sets = 0;
  mouse_present = fail_get = fail_set = false;
  enabled_mouse_keys = false;
  previous_mouse_keys_state = {};
}
int main() {
  reset(); mouse_present = true;
  enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && gets == 0 && sets == 0,
        "connected mouse leaves settings untouched");

  reset();
  enable_mouse_keys();
  check(enabled_mouse_keys && desktop.dwFlags == (MKF_MOUSEKEYSON | MKF_AVAILABLE),
        "missing mouse enables cursor workaround");
  restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys, "single enable restores exact settings");

  reset();
  for (int i = 0; i != 1200; ++i) enable_mouse_keys();
  restore_mouse_keys();
  check(desktop == original && gets == 1 && sets == 2,
        "per-frame checks preserve original baseline");

  reset();
  std::vector<std::thread> workers;
  for (int i = 0; i != 8; ++i) {
    workers.emplace_back([] { for (int j = 0; j != 100; ++j) enable_mouse_keys(); });
  }
  for (auto &worker : workers) worker.join();
  restore_mouse_keys();
  check(desktop == original && gets == 1 && sets == 2,
        "concurrent capture checks share one original snapshot");

  reset();
  enable_mouse_keys(); mouse_present = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original, "mouse reattachment still restores original settings");

  reset(); fail_get = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && sets == 0 && !enabled_mouse_keys,
        "failed snapshot never changes desktop");
  fail_get = false; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys, "snapshot failure can retry");

  reset(); fail_set = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && sets == 1 && !enabled_mouse_keys,
        "failed enable does not own desktop state");
  fail_set = false; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys, "failed enable can retry");

  reset(); enable_mouse_keys(); fail_set = true; restore_mouse_keys();
  check(enabled_mouse_keys && previous_mouse_keys_state == original,
        "failed restoration retains original snapshot");
  fail_set = false; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys,
        "later cleanup retries restoration without replacing original baseline");

  reset(); enable_mouse_keys(); restore_mouse_keys(); restore_mouse_keys();
  check(sets == 2 && desktop == original, "repeated cleanup is harmless");
  auto changed = original; changed.iMaxSpeed = 63; desktop = changed;
  enable_mouse_keys(); restore_mouse_keys();
  check(desktop == changed, "next session snapshots updated user preferences");

  auto existing = original; existing.dwFlags |= MKF_MOUSEKEYSON;
  reset(existing); enable_mouse_keys(); enable_mouse_keys(); restore_mouse_keys();
  check(desktop == existing, "existing accessibility preferences survive streaming");
  std::cout << cases << " Mouse Keys cases, " << failures << " failed\n";
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/platform/windows/misc.cpp')
    parser.add_argument('--compiler', default='/usr/bin/g++-15')
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index(';', source.index('using adapteraddrs_t =')) + 1
    globals_source = source[start:source.index('  HANDLE qos_handle', start)]
    enable = function(source, '  void enable_mouse_keys()')
    stop = function(source, '  void streaming_will_stop()')
    restore = 'void restore_mouse_keys() {\n' + stop[stop.index('    // Restore Mouse Keys'):]
    with tempfile.TemporaryDirectory(prefix='mouse-keys-test-') as directory:
        cpp = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        cpp.write_text(PRELUDE + globals_source + enable + '\n' + restore + TESTS)
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=undefined', str(cpp), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())

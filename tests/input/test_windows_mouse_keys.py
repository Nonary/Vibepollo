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
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
using namespace std::literals;
struct log_sink {
  template<class T> log_sink &operator<<(const T &) { return *this; }
};
#define BOOST_LOG(level) log_sink{}
using DWORD = unsigned;
using UINT = unsigned;
struct MOUSEKEYS {
  DWORD cbSize, dwFlags, iMaxSpeed, iTimeToMaxSpeed, iCtrlSpeed,
    dwReserved1, dwReserved2;
  bool operator==(const MOUSEKEYS &) const = default;
};
constexpr unsigned SM_MOUSEPRESENT = 1, SPI_GETMOUSEKEYS = 2,
  SPI_SETMOUSEKEYS = 3, MKF_MOUSEKEYSON = 1, MKF_AVAILABLE = 2;
std::mutex desktop_mutex;
MOUSEKEYS desktop;
// mouse_present is SM_MOUSEPRESENT, which goes stale when Windows misses a
// mouse arrival; raw_mice is what raw input lists.
bool mouse_present = false, fail_get = false, fail_set = false;
unsigned get_calls = 0, sets = 0;
int last_error = 5;
int GetSystemMetrics(unsigned) { return mouse_present; }
int GetLastError() { return last_error; }
int SystemParametersInfoW(unsigned action, unsigned, void *data, unsigned) {
  std::lock_guard lock(desktop_mutex);
  auto state = static_cast<MOUSEKEYS *>(data);
  if (action == SPI_GETMOUSEKEYS) {
    ++get_calls;
    if (fail_get) return 0;
    *state = desktop;
  } else {
    ++sets;
    if (fail_set) return 0;
    desktop = *state;
  }
  return 1;
}
constexpr DWORD RIM_TYPEMOUSE = 0, RIM_TYPEKEYBOARD = 1;
constexpr int ERROR_INSUFFICIENT_BUFFER = 122;
struct RAWINPUTDEVICELIST {
  void *hDevice;
  DWORD dwType;
};
unsigned raw_mice = 0;
bool raw_fail = false;
UINT GetRawInputDeviceList(RAWINPUTDEVICELIST *devices, UINT *count, UINT) {
  if (raw_fail) {
    last_error = 31;
    return static_cast<UINT>(-1);
  }
  const UINT total = raw_mice + 1;  // A keyboard is always listed.
  if (!devices) {
    *count = total;
    return 0;
  }
  if (*count < total) {
    *count = total;
    last_error = ERROR_INSUFFICIENT_BUFFER;
    return static_cast<UINT>(-1);
  }
  for (UINT i = 0; i != total; ++i) devices[i] = {nullptr, i < raw_mice ? RIM_TYPEMOUSE : RIM_TYPEKEYBOARD};
  return total;
}
// Delayed tasks run only when a test calls run_polls().
struct fake_task_pool {
  std::vector<std::function<void()>> delayed;
  template<class F, class Delay> void pushDelayed(F &&task, Delay) { delayed.emplace_back(std::forward<F>(task)); }
} task_pool;
std::filesystem::path record_directory;
std::filesystem::path appdata() { return record_directory; }
'''

TESTS = r'''
unsigned failures = 0, cases = 0;
void check(bool ok, const char *name) {
  ++cases;
  if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
const MOUSEKEYS original {sizeof(MOUSEKEYS), 0x40, 77, 3000, 5, 0, 0};
const MOUSEKEYS streaming {sizeof(MOUSEKEYS), MKF_MOUSEKEYSON | MKF_AVAILABLE, 10, 1000, 0, 0, 0};
bool record_exists() { return std::filesystem::exists(record_directory / "mouse_keys_restore.txt"); }
void run_polls() {
  auto due = std::move(task_pool.delayed);
  task_pool.delayed.clear();
  for (auto &task : due) task();
}
// A new host process: in-memory state and queued polls are gone, while the
// desktop's settings and the record on disk remain.
void restart_host() {
  enabled_mouse_keys = false;
  mouse_keys_restore_deferred = false;
  mouse_keys_restore_poll_scheduled = false;
  previous_mouse_keys_state = {};
  task_pool.delayed.clear();
}
void reset(MOUSEKEYS initial = original) {
  desktop = initial;
  get_calls = sets = 0;
  mouse_present = fail_get = fail_set = raw_fail = false;
  raw_mice = 0;
  restart_host();
  std::filesystem::remove(record_directory / "mouse_keys_restore.txt");
}
// A mouse arrived while Mouse Keys was on and Windows missed it, then the stream ended.
void defer_restore() {
  reset(); enable_mouse_keys(); raw_mice = 1; restore_mouse_keys();
}
bool deferred_on() {
  return enabled_mouse_keys && mouse_keys_restore_deferred && desktop == streaming && record_exists() &&
         task_pool.delayed.size() == 1;
}
bool released(const MOUSEKEYS &expected) {
  return desktop == expected && !enabled_mouse_keys && !mouse_keys_restore_deferred && !record_exists() &&
         task_pool.delayed.empty();
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  record_directory = argv[1];

  reset(); mouse_present = true;
  enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && get_calls == 0 && sets == 0 && !record_exists(),
        "connected mouse leaves settings untouched");

  reset();
  enable_mouse_keys();
  check(enabled_mouse_keys && desktop.dwFlags == (MKF_MOUSEKEYSON | MKF_AVAILABLE) && record_exists(),
        "missing mouse enables cursor workaround and records the original settings");
  restore_mouse_keys();
  check(released(original), "single enable restores exact settings and drops the record");

  reset();
  for (int i = 0; i != 1200; ++i) enable_mouse_keys();
  restore_mouse_keys();
  check(desktop == original && get_calls == 2 && sets == 2,
        "per-frame checks preserve original baseline");

  reset();
  std::vector<std::thread> workers;
  for (int i = 0; i != 8; ++i) {
    workers.emplace_back([] { for (int j = 0; j != 100; ++j) enable_mouse_keys(); });
  }
  for (auto &worker : workers) worker.join();
  restore_mouse_keys();
  check(desktop == original && get_calls == 2 && sets == 2,
        "concurrent capture checks share one original snapshot");

  reset();
  enable_mouse_keys(); mouse_present = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original, "mouse reattachment still restores original settings");

  reset(); fail_get = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && sets == 0 && !enabled_mouse_keys && !record_exists(),
        "failed snapshot never changes desktop");
  fail_get = false; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys, "snapshot failure can retry");

  reset(); fail_set = true; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && sets == 1 && !enabled_mouse_keys && !record_exists(),
        "failed enable does not own desktop state");
  fail_set = false; enable_mouse_keys(); restore_mouse_keys();
  check(desktop == original && !enabled_mouse_keys, "failed enable can retry");

  reset(); enable_mouse_keys(); fail_set = true; restore_mouse_keys();
  check(enabled_mouse_keys && previous_mouse_keys_state == original && record_exists(),
        "failed restoration retains original snapshot");
  fail_set = false; enable_mouse_keys(); restore_mouse_keys();
  check(released(original), "later cleanup retries restoration without replacing original baseline");

  reset(); enable_mouse_keys(); restore_mouse_keys(); restore_mouse_keys();
  check(sets == 2 && desktop == original, "repeated cleanup is harmless");
  auto changed = original; changed.iMaxSpeed = 63; desktop = changed;
  enable_mouse_keys(); restore_mouse_keys();
  check(desktop == changed, "next session snapshots updated user preferences");

  auto existing = original; existing.dwFlags |= MKF_MOUSEKEYSON;
  reset(existing); enable_mouse_keys(); enable_mouse_keys(); restore_mouse_keys();
  check(desktop == existing, "existing accessibility preferences survive streaming");

  // Deferred restoration.
  defer_restore();
  check(deferred_on(), "missed mouse arrival keeps Mouse Keys on, recorded, with a poll queued");
  run_polls();
  check(deferred_on(), "poll keeps Mouse Keys on while a mouse is listed");
  raw_mice = 0; run_polls();
  check(released(original), "poll restores once no mouse is listed");

  // An enumeration error is not an empty list.
  defer_restore(); raw_fail = true; run_polls();
  check(deferred_on(), "enumeration error while the mouse stays connected keeps Mouse Keys on");
  raw_mice = 0; run_polls();
  check(deferred_on(), "enumeration error never counts as a disconnected mouse");
  raw_fail = false; run_polls();
  check(released(original), "restoration waits for a successful empty enumeration");

  reset(); enable_mouse_keys(); raw_fail = true; restore_mouse_keys();
  check(deferred_on(), "enumeration error at stream end defers instead of restoring");
  raw_fail = false; run_polls();
  check(released(original), "deferral after an enumeration error restores once no mouse is listed");

  // Host restart.
  defer_restore(); restart_host();
  check(!enabled_mouse_keys && record_exists(), "record survives a host restart while deferred");
  recover_mouse_keys();
  check(deferred_on(), "restarted host resumes the deferral while the mouse stays connected");
  run_polls();
  check(deferred_on(), "resumed deferral keeps Mouse Keys on while a mouse is listed");
  raw_mice = 0; run_polls();
  check(released(original), "resumed deferral restores the original settings after mouse removal");

  reset(); enable_mouse_keys(); restart_host(); recover_mouse_keys();
  check(released(original), "host restart mid-stream restores the original settings at startup");

  defer_restore(); restart_host(); desktop = original; sets = 0; recover_mouse_keys();
  check(released(original) && sets == 0,
        "restart after Windows reset the settings drops the record without writing");

  reset(); recover_mouse_keys();
  check(desktop == original && sets == 0 && !enabled_mouse_keys, "startup without a record does nothing");

  reset();
  { std::ofstream corrupt {record_directory / "mouse_keys_restore.txt"}; corrupt << "garbage\n"; }
  recover_mouse_keys();
  check(desktop == original && sets == 0 && !record_exists() && !enabled_mouse_keys,
        "unreadable record is discarded");

  // Changes the user makes while restoration is deferred.
  defer_restore(); desktop.iMaxSpeed = 63; run_polls();
  check(enabled_mouse_keys && mouse_keys_restore_deferred && desktop.iMaxSpeed == 63 && task_pool.delayed.size() == 1,
        "poll keeps a user change while the mouse stays connected");
  raw_mice = 0; run_polls();
  auto user_speed = original; user_speed.iMaxSpeed = 63;
  check(released(user_speed), "mouse removal restores only the settings Sunshine still holds");

  defer_restore(); desktop = original; sets = 0; run_polls();
  check(released(original) && sets == 0, "user restoring the settings by hand ends the deferral without writing");

  defer_restore(); desktop.iTimeToMaxSpeed = 2500; restart_host(); recover_mouse_keys();
  check(enabled_mouse_keys && mouse_keys_restore_deferred && desktop.iTimeToMaxSpeed == 2500 &&
          desktop.dwFlags == streaming.dwFlags && record_exists() && task_pool.delayed.size() == 1,
        "restarted host keeps a user change while the mouse stays connected");
  raw_mice = 0; run_polls();
  auto user_time = original; user_time.iTimeToMaxSpeed = 2500;
  check(released(user_time), "restarted host restores only the settings Sunshine still holds");

  std::cout << cases << " Mouse Keys cases, " << failures << " failed\n";
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/platform/windows/misc.cpp')
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index(';', source.index('using adapteraddrs_t =')) + 1
    globals_source = source[start:source.index('  HANDLE qos_handle', start)]
    helpers = function(source, '  namespace {\n    MOUSEKEYS streaming_mouse_keys_state()')
    recover = function(source, '  void recover_mouse_keys()')
    enable = function(source, '  void enable_mouse_keys()')
    stop = function(source, '  void streaming_will_stop()')
    restore = 'void restore_mouse_keys() {\n' + stop[stop.index('    // Restore Mouse Keys'):]
    with tempfile.TemporaryDirectory(prefix='mouse-keys-test-') as directory:
        cpp = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        records = Path(directory) / 'appdata'
        records.mkdir()
        cpp.write_text(PRELUDE + globals_source + helpers + '\n' + recover + '\n' + enable + '\n' + restore + TESTS)
        command = [args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread']
        try:
            subprocess.run(command + ['-fsanitize=undefined', str(cpp), '-o', str(executable)],
                           check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as error:
            if 'cannot find -lubsan' not in error.stderr:
                print(error.stderr)
                raise
            print('UBSan runtime unavailable; running functional assertions without sanitizer')
            subprocess.run(command + [str(cpp), '-o', str(executable)], check=True)
        return subprocess.run([str(executable), str(records)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())

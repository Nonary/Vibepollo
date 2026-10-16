// The production globals/helpers/callbacks are extracted without rewriting.
// These fake boundaries never invoke Windows or change the test host's state.
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::literals;
using ULONG = std::uint32_t;
using DWORD = std::uint32_t;
using NTSTATUS = std::int32_t;
using BOOL = int;
using BOOLEAN = unsigned char;
using HANDLE = void *;
using HMODULE = void *;
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr DWORD ERROR_SUCCESS = 0, TIMERR_NOERROR = 0;
constexpr DWORD HIGH_PRIORITY_CLASS = 0x80, NORMAL_PRIORITY_CLASS = 0x20;
constexpr DWORD ABOVE_NORMAL_PRIORITY_CLASS = 0x8000, BELOW_NORMAL_PRIORITY_CLASS = 0x4000;
constexpr DWORD IDLE_PRIORITY_CLASS = 0x40, REALTIME_PRIORITY_CLASS = 0x100;
constexpr int LOAD_LIBRARY_SEARCH_SYSTEM32 = 0, SM_MOUSEPRESENT = 0;
constexpr int SPI_GETMOUSEKEYS = 1, SPI_SETMOUSEKEYS = 2;
constexpr int MKF_MOUSEKEYSON = 1, MKF_AVAILABLE = 2;
constexpr int wlan_interface_state_connected = 1, wlan_intf_opcode_media_streaming_mode = 1;
#define NT_SUCCESS(status) ((status) >= 0)
#define WLAN_API_MAKE_VERSION(major, minor) ((major) | ((minor) << 16))
#define BOOST_LOG(level) log_output

std::ostringstream log_output;
struct MOUSEKEYS {
  DWORD cbSize = 0, dwFlags = 0, iMaxSpeed = 0, iTimeToMaxSpeed = 0;
};
struct WLAN_INTERFACE_INFO {
  int isState = 0, InterfaceGuid = 0;
};
struct WLAN_INTERFACE_INFO_LIST {
  DWORD dwNumberOfItems = 0;
  WLAN_INTERFACE_INFO InterfaceInfo[1];
};
using PWLAN_INTERFACE_INFO_LIST = WLAN_INTERFACE_INFO_LIST *;
void QOSCreateHandle();
void QOSAddSocketToFlow();
void QOSRemoveSocketFromFlow();

struct {
  ULONG maximum = 5000;
  int query_failures = 0, native_set_failures = 0, native_release_failures = 0;
  int begin_failures = 0, end_failures = 0, priority_query_failures = 0, priority_set_failures = 0;
  int query_calls = 0, begin_calls = 0, end_calls = 0, winmm_owned = 0;
  int priority_queries = 0, invalid_releases = 0;
  std::atomic<int> dwm_starts = 0, dwm_stops = 0;
  std::atomic<int> watchdog_starts = 0, watchdog_stops = 0;
  DWORD priority = ABOVE_NORMAL_PRIORITY_CLASS;
  std::vector<std::pair<ULONG, BOOLEAN>> native_calls;
  // Native requests belong to a process: another TRUE replaces its request.
  // Other processes' requests remain independent of this process's FALSE.
  std::optional<ULONG> native_request, foreign_native_request;
  std::vector<DWORD> priority_sets;
  bool mouse_present = true, paused = false;
  MOUSEKEYS mouse_keys {sizeof(MOUSEKEYS), 0x80, 37, 420};
  int mouse_gets = 0, mouse_sets = 0;
} os;

bool fail(int &remaining) {
  if (!remaining) {
    return false;
  }
  --remaining;
  return true;
}
NTSTATUS NtQueryTimerResolution(ULONG *minimum, ULONG *maximum, ULONG *current) {
  ++os.query_calls;
  *minimum = 156250;
  *maximum = os.maximum;
  *current = 156250;
  return fail(os.query_failures) ? -1 : 0;
}
NTSTATUS NtSetTimerResolution(ULONG desired, BOOLEAN acquire, ULONG *current) {
  os.native_calls.emplace_back(desired, acquire);
  *current = desired;
  if (fail(acquire ? os.native_set_failures : os.native_release_failures)) {
    return -1;
  }
  if (acquire) {
    os.native_request = desired;
  } else if (os.native_request) {
    os.native_request.reset();
  } else {
    ++os.invalid_releases;
    return -1;
  }
  return 0;
}
DWORD timeBeginPeriod(unsigned period) {
  ++os.begin_calls;
  if (period != 1 || fail(os.begin_failures)) {
    return 1;
  }
  ++os.winmm_owned;
  return TIMERR_NOERROR;
}
DWORD timeEndPeriod(unsigned period) {
  ++os.end_calls;
  if (period != 1 || fail(os.end_failures)) {
    return 1;
  }
  if (!os.winmm_owned) {
    ++os.invalid_releases;
    return 1;
  }
  --os.winmm_owned;
  return TIMERR_NOERROR;
}
HANDLE GetCurrentProcess() { return &os; }
DWORD GetPriorityClass(HANDLE) {
  ++os.priority_queries;
  return fail(os.priority_query_failures) ? 0 : os.priority;
}
BOOL SetPriorityClass(HANDLE, DWORD priority) {
  os.priority_sets.push_back(priority);
  if (fail(os.priority_set_failures)) {
    return FALSE;
  }
  os.priority = priority;
  return TRUE;
}
DWORD GetLastError() { return 5; }
HMODULE LoadLibraryExA(const char *, void *, int) { return nullptr; }
void *GetProcAddress(HMODULE, const char *) { return nullptr; }
BOOL FreeLibrary(HMODULE) { return TRUE; }
void DwmEnableMMCSS(BOOL enabled) { ++(enabled ? os.dwm_starts : os.dwm_stops); }
DWORD WlanOpenHandle(DWORD, void *, DWORD *, HANDLE *) { return 1; }
DWORD WlanCloseHandle(HANDLE, void *) { return ERROR_SUCCESS; }
void WlanFreeMemory(void *) {}
DWORD WlanEnumInterfaces(HANDLE, void *, PWLAN_INTERFACE_INFO_LIST *) { return 1; }
DWORD WlanSetInterface(HANDLE, const int *, int, DWORD, const void *, void *) { return 1; }
int GetSystemMetrics(int) { return os.mouse_present; }
BOOL SystemParametersInfoW(int action, unsigned, void *value, unsigned) {
  if (action == SPI_GETMOUSEKEYS) {
    ++os.mouse_gets;
    *static_cast<MOUSEKEYS *>(value) = os.mouse_keys;
  } else {
    ++os.mouse_sets;
    os.mouse_keys = *static_cast<MOUSEKEYS *>(value);
  }
  return TRUE;
}
namespace display_helper_integration {
  void start_watchdog() { ++os.watchdog_starts; }
  void stop_watchdog() { ++os.watchdog_stops; }
}
namespace proc {
  struct {
    int current_app_id() { return os.paused ? 1 : 0; }
  } proc;
}
struct {
  bool load() { return false; }
  bool owning_undo_file() { return false; }
  void restore_from_and_delete_undo_file_if_exists() {}
  void modify_application_profile() {}
  void modify_global_profile() {}
  void unload() {}
} nvprefs_instance;

#include "production.inc"

int failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
int native_count() {
  return static_cast<int>(os.native_request.has_value()) + static_cast<int>(os.foreign_native_request.has_value());
}
void clean(int foreign_native, int foreign_winmm) {
  check(native_count() == foreign_native && os.winmm_owned == foreign_winmm,
        "only successful owned timer acquisitions are released");
  check(os.invalid_releases == 0, "no release without ownership");
}
void start() { platf::streaming_will_start(); }
void stop() { platf::streaming_will_stop(); }

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string scenario = argv[1];
  int foreign_native = 0, foreign_winmm = 0;
  if (scenario == "native") {
    start();
    os.maximum = 10000; // Teardown must use the saved request without requerying.
    stop();
    check(os.native_calls == std::vector<std::pair<ULONG, BOOLEAN>> {{5000, TRUE}, {5000, FALSE}},
          "native release uses FALSE and the acquired resolution");
    check(os.query_calls == 1 && !os.end_calls, "native release neither requeries nor ends WinMM");
  } else if (scenario == "multi-owner" || scenario == "winmm-multi-owner" || scenario == "threaded-owners") {
    if (scenario == "winmm-multi-owner") os.query_failures = 1;
    if (scenario == "threaded-owners") {
      std::barrier ready(3), leave(3);
      auto owner = [&] { start(); ready.arrive_and_wait(); leave.arrive_and_wait(); stop(); };
      std::thread first(owner), second(owner);
      ready.arrive_and_wait();
      check(native_count() == 1, "concurrent owners acquire one scheduling request");
      leave.arrive_and_wait();
      first.join(); second.join();
    } else {
      start(); start(); stop();
      check(native_count() + os.winmm_owned == 1 && os.priority == HIGH_PRIORITY_CLASS,
            "first owner cannot restore shared scheduling");
      stop();
    }
    check(scenario == "winmm-multi-owner" ? os.begin_calls == 1 && os.end_calls == 1 : os.native_calls.size() == 2,
          "last owner performs one scheduling cleanup");
    check(os.priority == ABOVE_NORMAL_PRIORITY_CLASS, "last owner restores original priority");
  } else if (scenario == "query-fallback" || scenario == "set-fallback" || scenario == "no-timer") {
    if (scenario == "set-fallback") os.native_set_failures = 1;
    else os.query_failures = 1;
    if (scenario == "no-timer") os.begin_failures = 1;
    start(); stop();
    check(os.begin_calls == 1 && os.end_calls == (scenario == "no-timer" ? 0 : 1),
          "WinMM release requires a successful matching begin");
  } else if (scenario == "native-release-retry" || scenario == "winmm-release-retry" ||
             scenario == "native-release-new-cycle" || scenario == "winmm-release-new-cycle") {
    const bool winmm = scenario.starts_with("winmm");
    if (winmm) { os.query_failures = 1; os.end_failures = 1; }
    else os.native_release_failures = 1;
    start(); stop();
    check(native_count() + os.winmm_owned == 1, "failed release retains the original request");
    if (scenario.ends_with("new-cycle")) {
      start();
      check(os.query_calls == 1 && os.begin_calls == (winmm ? 1 : 0),
            "new cycle reuses pending timer ownership without acquiring again");
    }
    stop();
    const auto attempts = winmm ? os.end_calls : static_cast<int>(os.native_calls.size()) - 1;
    check(attempts == 2, "failed cleanup retries the same backend");
  } else if (scenario == "priority-query-fails" || scenario == "priority-set-fails") {
    if (scenario == "priority-query-fails") os.priority_query_failures = 1;
    else os.priority_set_failures = 1;
    start(); stop();
    check(os.priority == ABOVE_NORMAL_PRIORITY_CLASS, "failed promotion never changes the prior class");
    check(os.priority_sets.size() == (scenario == "priority-query-fails" ? 0u : 1u),
          "failed promotion creates no restoration obligation");
  } else if (scenario == "priority-restore-retry" || scenario == "priority-restore-new-cycle" ||
             scenario == "priority-cleanup-query-fails") {
    start();
    if (scenario == "priority-cleanup-query-fails") os.priority_query_failures = 1;
    else os.priority_set_failures = 1;
    stop();
    check(os.priority == HIGH_PRIORITY_CLASS, "failed restoration remains pending");
    if (scenario.ends_with("new-cycle")) start();
    stop();
    check(os.priority == ABOVE_NORMAL_PRIORITY_CLASS, "retry restores the first snapshot");
  } else if (scenario == "external-priority" || scenario == "external-priority-after-failure") {
    start();
    const bool pending = scenario.ends_with("after-failure");
    if (pending) { os.priority_set_failures = 1; stop(); }
    os.priority = BELOW_NORMAL_PRIORITY_CLASS; stop();
    check(os.priority == BELOW_NORMAL_PRIORITY_CLASS && os.priority_sets.size() == (pending ? 2u : 1u),
          "cleanup preserves a later external priority class");
    start(); stop();
    check(os.priority == BELOW_NORMAL_PRIORITY_CLASS, "later cycle snapshots the new class");
  } else if (scenario == "foreign-native" || scenario == "foreign-winmm") {
    if (scenario == "foreign-native") {
      foreign_native = 1;
      os.foreign_native_request = 10000;
    } else {
      foreign_winmm = 1;
      os.winmm_owned = 1;
      os.query_failures = 1;
    }
    start(); stop(); stop();
    check(os.foreign_native_request.has_value() == static_cast<bool>(foreign_native) && os.winmm_owned == foreign_winmm,
          "duplicate cleanup preserves unrelated timer requests");
  } else if (scenario == "unmatched-stop") {
    stop(); stop();
    check(os.native_calls.empty() && !os.begin_calls && !os.end_calls && os.priority_sets.empty(),
          "unmatched stops never mutate scheduling without ownership");
    start(); stop();
    check(os.priority == ABOVE_NORMAL_PRIORITY_CLASS, "unmatched stops do not underflow owner count");
  } else if (scenario == "balanced-cycles" || scenario == "mixed-backends") {
    start(); stop();
    if (scenario == "mixed-backends") os.query_failures = 1;
    start(); stop(); start(); stop();
    check(os.query_calls == 3 && os.priority == ABOVE_NORMAL_PRIORITY_CLASS,
          "each balanced cycle has one fresh snapshot and timer acquisition");
    check(os.begin_calls == (scenario == "mixed-backends" ? 1 : 0), "backend changes leave no stale ownership");
  } else if (scenario == "mouse-keys") {
    os.mouse_present = false;
    const auto original = os.mouse_keys;
    start(); platf::enable_mouse_keys(); platf::enable_mouse_keys();
    stop();
    check(os.mouse_keys.dwFlags == original.dwFlags && os.mouse_keys.iMaxSpeed == original.iMaxSpeed &&
          os.mouse_keys.iTimeToMaxSpeed == original.iTimeToMaxSpeed && os.mouse_gets == 1,
          "final cleanup preserves existing Mouse Keys snapshot behavior");
  } else if (scenario == "paused-watchdog") {
    start(); os.paused = true; stop();
    check(os.watchdog_starts == 1 && os.watchdog_stops == 0, "paused app keeps the display helper watchdog");
  } else if (scenario.starts_with("priority-")) {
    const std::map<std::string, DWORD> priorities {
      {"priority-normal", NORMAL_PRIORITY_CLASS}, {"priority-above-normal", ABOVE_NORMAL_PRIORITY_CLASS},
      {"priority-below-normal", BELOW_NORMAL_PRIORITY_CLASS}, {"priority-idle", IDLE_PRIORITY_CLASS},
      {"priority-high", HIGH_PRIORITY_CLASS}, {"priority-realtime", REALTIME_PRIORITY_CLASS},
    };
    const auto original = priorities.at(scenario);
    os.priority = original;
    start(); stop();
    check(os.priority == original, "restore the actual prior priority class");
    check(os.priority_sets.size() == (original == HIGH_PRIORITY_CLASS ? 0u : 2u),
          "already HIGH has no priority change to restore");
  } else {
    std::cerr << "Unknown scenario: " << scenario << '\n';
    return 2;
  }
  clean(foreign_native, foreign_winmm);
  if (failures) std::cerr << scenario << ": " << failures << " failed assertions\n";
  return failures ? 1 : 0;
}

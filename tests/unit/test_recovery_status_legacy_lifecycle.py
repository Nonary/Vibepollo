"""Run production legacy observer and teardown paths with narrow Win32 seams.

The extracted DisplayEventPump class is production code. Stubbed Win32 calls
provide repeatable messages and record the native window's parent argument;
the actual Windows broadcast delivery semantics still need native coverage.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++"
legacy = (root / "tools/display_settings_helper.cpp").read_text()


def block_after(source, marker, occurrence=0):
    start = -1
    for _ in range(occurrence + 1):
        start = source.index(marker, start + 1)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def function(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(map(re.escape, tokens)), source)
    if match is None:
        raise AssertionError(f"Production function not found: {signature}")
    return block_after(source, source[match.start():source.index("{", match.start())].strip())


def lambda_body(source, marker):
    start = source.index(marker) + len(marker)
    while source[start].isspace():
        start += 1
    assert source[start] == "{"
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


event_callback = lambda_body(legacy, "event_pump.start([this](const char *event_reason) ")
event_callback = "{ ++event_callback_count; " + event_callback[1:]
notify = function(legacy, "void notify_recovery_status_locked()")
monitor_guid = re.search(r"static const GUID kMonitorInterfaceGuid = .*?;", legacy).group()
event_pump = block_after(legacy, "class DisplayEventPump") + ";"
terminal_blocks = [block_after(legacy, "if (!restore_attempt_completed)", index) for index in range(legacy.count("if (!restore_attempt_completed)"))]
assert len(terminal_blocks) == 2
lifecycle_start = legacy.index("    // Ensure the worker thread is stopped and the server handle is")
lifecycle_end = legacy.index("    async_pipe.stop();", lifecycle_start) + len("    async_pipe.stop();")
lifecycle = legacy[lifecycle_start:lifecycle_end]

success_sequences = []
search_from = legacy.index("static void restore_poll_proc")
for _ in range(2):
    begin = legacy.index("const bool exit_helper = self->should_exit_after_restore();", search_from)
    end = legacy.index("return;", begin) + len("return;")
    success_sequences.append(legacy[begin:end])
    search_from = end

program = r'''
#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "src/platform/windows/recovery_status.h"

using DWORD = std::uint32_t;
using UINT = unsigned int;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using LONG_PTR = std::intptr_t;
using BOOL = int;
using HINSTANCE = void *;
using HWND = void *;
using HPOWERNOTIFY = void *;
using HDEVNOTIFY = void *;
#define CALLBACK
constexpr BOOL TRUE = 1;
constexpr UINT WM_NCCREATE = 0x0081, WM_DISPLAYCHANGE = 0x007e, WM_DEVICECHANGE = 0x0219;
constexpr UINT WM_POWERBROADCAST = 0x0218, WM_DESTROY = 0x0002, WM_CLOSE = 0x0010;
constexpr UINT GWLP_USERDATA = -21, DBT_DEVNODES_CHANGED = 7, DBT_DEVICEARRIVAL = 0x8000;
constexpr UINT DBT_DEVICEREMOVECOMPLETE = 0x8004, PBT_APMRESUMEAUTOMATIC = 0x0012;
constexpr UINT PBT_POWERSETTINGCHANGE = 0x8013, DEVICE_NOTIFY_WINDOW_HANDLE = 0;
constexpr UINT DBT_DEVTYP_DEVICEINTERFACE = 5;
struct GUID { std::uint32_t Data1; std::uint16_t Data2; std::uint16_t Data3; std::uint8_t Data4[8]; };
inline bool operator==(const GUID &left, const GUID &right) {
  return left.Data1 == right.Data1 && left.Data2 == right.Data2 && left.Data3 == right.Data3 &&
         std::equal(std::begin(left.Data4), std::end(left.Data4), std::begin(right.Data4));
}
inline const GUID GUID_MONITOR_POWER_ON {1, 0, 0, {0}};
struct POWERBROADCAST_SETTING { GUID PowerSetting; DWORD DataLength; DWORD Data[1]; };
struct DEV_BROADCAST_DEVICEINTERFACE_W { DWORD dbcc_size; DWORD dbcc_devicetype; GUID dbcc_classguid; wchar_t dbcc_name[1]; };
struct CREATESTRUCTW { void *lpCreateParams; };
struct MSG { UINT message; WPARAM wParam; LPARAM lParam; };
using WNDPROC = LRESULT (CALLBACK *)(HWND, UINT, WPARAM, LPARAM);
struct WNDCLASSEXW { UINT cbSize; WNDPROC lpfnWndProc; HINSTANCE hInstance; const wchar_t *lpszClassName; };
namespace win_stub {
  inline std::mutex mutex;
  inline std::deque<MSG> messages;
  inline WNDPROC wnd_proc = nullptr;
  inline void *user_data = nullptr;
  inline HWND hwnd = reinterpret_cast<HWND>(0x1);
  inline HWND parent = reinterpret_cast<HWND>(0x1);
  inline void enqueue(MSG message) { std::lock_guard lock(mutex); messages.push_back(message); }
  inline void reset() {
    std::lock_guard lock(mutex);
    messages.clear(); wnd_proc = nullptr; user_data = nullptr; parent = reinterpret_cast<HWND>(0x1);
  }
}
inline HINSTANCE GetModuleHandleW(const wchar_t *) { return nullptr; }
inline unsigned short RegisterClassExW(const WNDCLASSEXW *wc) { win_stub::wnd_proc = wc->lpfnWndProc; return 1; }
inline HWND CreateWindowExW(DWORD, const wchar_t *, const wchar_t *, DWORD, int, int, int, int,
                           HWND parent, void *, HINSTANCE, void *parameter) {
  win_stub::parent = parent;
  CREATESTRUCTW create {parameter};
  win_stub::wnd_proc(win_stub::hwnd, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&create));
  return win_stub::hwnd;
}
inline LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR value) { win_stub::user_data = reinterpret_cast<void *>(value); return 0; }
inline LONG_PTR GetWindowLongPtrW(HWND, int) { return reinterpret_cast<LONG_PTR>(win_stub::user_data); }
inline LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline BOOL GetMessageW(MSG *message, HWND, UINT, UINT) {
  std::lock_guard lock(win_stub::mutex);
  if (win_stub::messages.empty()) return 0;
  *message = win_stub::messages.front(); win_stub::messages.pop_front(); return 1;
}
inline BOOL TranslateMessage(const MSG *) { return 1; }
inline LRESULT DispatchMessageW(const MSG *message) {
  return win_stub::wnd_proc(win_stub::hwnd, message->message, message->wParam, message->lParam);
}
inline BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return 1; }
inline void PostQuitMessage(int) {}
inline HPOWERNOTIFY RegisterPowerSettingNotification(HWND, const GUID *, DWORD) { return reinterpret_cast<void *>(0x2); }
inline HDEVNOTIFY RegisterDeviceNotificationW(HWND, void *, DWORD) { return reinterpret_cast<void *>(0x3); }
inline BOOL UnregisterPowerSettingNotification(HPOWERNOTIFY) { return 1; }
inline BOOL UnregisterDeviceNotification(HDEVNOTIFY) { return 1; }
inline BOOL DestroyWindow(HWND) { return 1; }
inline BOOL UnregisterClassW(const wchar_t *, HINSTANCE) { return 1; }
''' + monitor_guid + r'''

namespace legacy {
''' + event_pump + r'''
  enum class RestoreWindow { Event, Primary };
  class ObserverState {
  public:
    DisplayEventPump event_pump;
    std::atomic<bool> disconnect_settlement_pending {false}, restore_requested {false}, restore_poll_active {false};
    std::atomic<std::int64_t> recovery_status_failed_at_ms {100};
    std::int64_t now_ms = 2000;
    std::mutex recovery_status_mutex;
    display_helper::recovery_status::policy recovery_status;
    std::function<void(std::uint64_t, display_helper::recovery_status::status, std::uint64_t)> recovery_notification;
    unsigned polling_starts = 0, signaled_events = 0;
    std::atomic<unsigned> event_callback_count {0};
    std::int64_t steady_now_ms() const { return now_ms; }
    std::uint64_t current_connection_epoch() const { return 3; }
    void ensure_restore_polling(RestoreWindow, const char *, bool) { ++polling_starts; }
    void signal_restore_event(const char *, RestoreWindow) { ++signaled_events; }
''' + notify + r'''
    void start_observer() {
      event_pump.start([this](const char *event_reason) ''' + event_callback + r''');
    }
  };
  void wait_for_callbacks(ObserverState &state, unsigned expected) {
    for (unsigned attempt = 0; attempt < 1000 && state.event_callback_count.load() < expected; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(state.event_callback_count.load() >= expected);
  }
}

struct SimplePump { unsigned stops = 0; void stop() { ++stops; } };
struct TerminalState { SimplePump event_pump; std::atomic<bool> event_pump_running {true}; };
static void cleanup_failed_restore(TerminalState *self, bool restore_attempt_completed) {
''' + terminal_blocks[0] + r'''
}
static void cleanup_unresolved_restore(TerminalState *self, bool restore_attempt_completed) {
''' + terminal_blocks[1] + r'''
}

struct TeardownState {
  std::atomic<bool> command_worker_stop {false};
  std::condition_variable command_queue_cv;
  std::jthread command_worker;
  std::mutex command_queue_mutex;
  std::deque<int> command_queue;
  std::mutex recovery_status_mutex;
  std::function<void()> recovery_notification;
  bool heartbeat_ended = false;
  void end_heartbeat_monitoring() { heartbeat_ended = true; }
};
struct TeardownPipe {
  TeardownState *state;
  bool stopped = false;
  void stop() {
    assert(state->heartbeat_ended);
    assert(state->command_worker_stop.load());
    assert(!state->command_worker.joinable());
    assert(state->command_queue.empty());
    assert(!state->recovery_notification);
    stopped = true;
  }
};
static void teardown(TeardownState &state, TeardownPipe &async_pipe) {
''' + lifecycle + r'''
}

static POWERBROADCAST_SETTING power_setting(DWORD data, DWORD length = sizeof(DWORD)) {
  POWERBROADCAST_SETTING setting {};
  setting.PowerSetting = GUID_MONITOR_POWER_ON;
  setting.DataLength = length;
  setting.Data[0] = data;
  return setting;
}

// Execute each production terminal success sequence with pipe retirement
// modeled by running_flag. Both paths must publish while the pipe stays live.
struct SuccessfulRestoreState {
  std::atomic<bool> running {true};
  std::atomic<bool> *running_flag {&running};
  std::atomic<bool> event_pump_running {true}, restore_poll_active {true}, restore_requested {true};
  std::atomic<int> restore_active_until_ms {1}, last_restore_event_ms {1};
  enum class RestoreWindow { Event };
  std::atomic<RestoreWindow> restore_active_window {RestoreWindow::Event};
  struct Pump { void stop() {} } event_pump;
  bool should_exit = true;
  unsigned notices = 0;
  bool should_exit_after_restore() const { return should_exit; }
  void clear_restore_origin() {}
  void publish_recovery_status(display_helper::recovery_status::status value,
                               std::uint64_t ticket, std::uint64_t generation, std::uint64_t epoch) {
    assert(running.load() && !restore_poll_active.load() && !event_pump_running.load());
    assert(value == display_helper::recovery_status::status::restored && ticket == 7 && generation == 9 && epoch == 3);
    ++notices;
  }
};
struct SuccessLog { template<class T> SuccessLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) SuccessLog {}
void initial_restore_success(SuccessfulRestoreState *self) {
  using RestoreWindow = SuccessfulRestoreState::RestoreWindow;
  const std::uint64_t last_attempt_ticket = 7, guard_generation = 9, status_epoch = 3;
''' + success_sequences[0] + r'''
}
void retry_restore_success(SuccessfulRestoreState *self) {
  using RestoreWindow = SuccessfulRestoreState::RestoreWindow;
  const std::uint64_t last_attempt_ticket = 7, guard_generation = 9, status_epoch = 3;
''' + success_sequences[1] + r'''
}

int main() {
  using namespace std::chrono_literals;
  for (const auto success : {initial_restore_success, retry_restore_success}) {
    SuccessfulRestoreState state;
    success(&state); assert(state.notices == 1 && !state.running.load());
    SuccessfulRestoreState newer_connection;
    newer_connection.should_exit = false;
    success(&newer_connection); assert(newer_connection.notices == 1 && newer_connection.running.load());
  }
  {
    TerminalState state;
    cleanup_failed_restore(&state, false);
    assert(state.event_pump.stops == 1 && !state.event_pump_running.load());
    cleanup_failed_restore(&state, true);
    assert(state.event_pump.stops == 1);
    cleanup_unresolved_restore(&state, false);
    assert(state.event_pump.stops == 2);
    cleanup_unresolved_restore(&state, true);
    assert(state.event_pump.stops == 2);
  }
  {
    TeardownState state;
    state.command_queue.push_back(1);
    state.recovery_notification = [] {};
    state.command_worker = std::jthread([](std::stop_token) { std::this_thread::sleep_for(5ms); });
    TeardownPipe pipe {&state};
    teardown(state, pipe);
    assert(pipe.stopped);
  }
  {
    legacy::ObserverState observer;
    observer.recovery_status.begin(7, 9, 3);
    assert(observer.recovery_status.publish(display_helper::recovery_status::status::failed, 7, 9, 3));
    std::vector<std::tuple<std::uint64_t, display_helper::recovery_status::status, std::uint64_t>> notices;
    observer.recovery_notification = [&](auto ticket, auto status, auto event) { notices.emplace_back(ticket, status, event); };

    win_stub::reset();
    observer.now_ms = 2000;
    auto display_msg = MSG {WM_DISPLAYCHANGE, 0, 0};
    win_stub::enqueue(display_msg);
    observer.start_observer();
    legacy::wait_for_callbacks(observer, 1);
    observer.event_pump.stop();
    assert(win_stub::parent == nullptr); // Production creates a hidden top-level window.
    assert(observer.event_callback_count.load() == 1);
    assert(observer.recovery_status.event_revision() == 1);
    assert(notices.size() == 1 && std::get<2>(notices.back()) == 1);

    win_stub::reset();
    auto off = power_setting(0);
    auto on = power_setting(1);
    auto repeated_on = power_setting(1);
    auto invalid = power_setting(1, 1);
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&off)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&on)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&repeated_on)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&invalid)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&on)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&off)});
    win_stub::enqueue({WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&on)});
    observer.start_observer();
    legacy::wait_for_callbacks(observer, 2);
    observer.event_pump.stop();
    assert(notices.size() == 3);
    assert(std::get<2>(notices[1]) == 2 && std::get<2>(notices[2]) == 3);
    assert(win_stub::parent == nullptr);

    win_stub::reset();
    observer.now_ms = 1100; // Inside the 1500ms post-failure quiet window.
    win_stub::enqueue(display_msg);
    observer.start_observer();
    legacy::wait_for_callbacks(observer, 3);
    observer.event_pump.stop();
    assert(observer.recovery_status.event_revision() == 3 && notices.size() == 3);
  }
}
'''

with tempfile.TemporaryDirectory(prefix="recovery-status-legacy-lifecycle-") as temporary:
    source = pathlib.Path(temporary) / "legacy_lifecycle.cpp"
    binary = pathlib.Path(temporary) / "legacy_lifecycle"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

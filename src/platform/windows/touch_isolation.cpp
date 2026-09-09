/**
 * @file src/platform/windows/touch_isolation.cpp
 * @brief Low-level mouse hook that suppresses touch-to-mouse promotion.
 */
#ifdef _WIN32
  #define WINVER 0x0A00

  #include "touch_isolation.h"

  // winsock2.h must precede Windows.h, and both must precede any local header that
  // reaches for a Win32 type. clang-format would sort these apart; leave them alone.
  #include <winsock2.h>
  #include <Windows.h>

  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cwchar>
  #include <mutex>
  #include <optional>
  #include <thread>
  #include <vector>

using namespace std::literals;

namespace {
  namespace ti = platf::touch_isolation;

  constexpr UINT kMsgResolve = WM_APP + 1;

  // The input desktop changes under us on UAC prompts and the lock screen, and a
  // low-level hook only ever sees the desktop its thread is attached to. Re-check often
  // enough that touching right after unlocking works.
  constexpr UINT kWatchdogIntervalMs = 1000;

  // One slot per streaming client, because `virtual_display_mode = per_client` gives
  // each client its own display. Publishing a bounding box instead would be cheaper,
  // but the isolated layouts park a virtual display at 64000,64000, so a box spanning
  // two of them would cover the whole desktop.
  constexpr std::size_t MAX_SLOTS = 4;

  constexpr auto CURSOR_POKE_INTERVAL = 100ms;

  // MinGW's winuser.h predates the flag; its value is stable across Windows releases.
  #ifndef CURSOR_SUPPRESSED
    #define CURSOR_SUPPRESSED 0x00000002
  #endif

  std::mutex &state_mutex() {
    static std::mutex m;
    return m;
  }

  struct slot_t {
    bool used = false;
    ti::rect_t requested {};
  };

  // Guarded by state_mutex().
  slot_t g_slots[MAX_SLOTS] {};
  DWORD g_thread_id = 0;
  bool g_thread_started = false;

  // Published for the hook to read without locking. Each rectangle is two packed
  // halves, so a publish is two stores; a reader catching them mid-update mis-filters
  // at most one event, which is harmless.
  std::atomic<std::uint64_t> g_published_lt[MAX_SLOTS] {};
  std::atomic<std::uint64_t> g_published_rb[MAX_SLOTS] {};
  std::atomic<bool> g_published[MAX_SLOTS] {};
  std::atomic<bool> g_any_published {false};
  std::atomic<std::uint64_t> g_swallowed {0};

  // Isolation thread only.
  HHOOK g_hook = nullptr;
  UINT_PTR g_timer_id = 0;
  std::wstring g_hook_desktop_name;

  std::uint64_t pack(const int high, const int low) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(low));
  }

  ti::rect_t load_published(const std::size_t slot) {
    const auto lt = g_published_lt[slot].load(std::memory_order_relaxed);
    const auto rb = g_published_rb[slot].load(std::memory_order_relaxed);
    const auto high = [](const std::uint64_t value) {
      return static_cast<int>(static_cast<std::uint32_t>(value >> 32));
    };
    const auto low = [](const std::uint64_t value) {
      return static_cast<int>(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    };
    return ti::rect_t {high(lt), low(lt), high(rb), low(rb)};
  }

  /**
   * @brief Decides whether to drop a promoted mouse event.
   * @details Runs on the OS input path. Windows silently detaches a low-level hook
   * whose callback exceeds `LowLevelHooksTimeout`, so this must stay allocation-free,
   * lock-free, and free of logging.
   */
  LRESULT CALLBACK hook_proc(const int nCode, const WPARAM wParam, const LPARAM lParam) {
    if (nCode == HC_ACTION && g_any_published.load(std::memory_order_relaxed)) {
      const auto *event = reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
      const auto extra_info = static_cast<std::uint64_t>(event->dwExtraInfo);

      if (ti::is_promoted_from_pointer(extra_info)) {
        for (std::size_t slot = 0; slot < MAX_SLOTS; ++slot) {
          if (!g_published[slot].load(std::memory_order_relaxed)) {
            continue;
          }
          if (ti::should_swallow(extra_info, event->pt.x, event->pt.y, load_published(slot))) {
            g_swallowed.fetch_add(1, std::memory_order_relaxed);
            return 1;
          }
        }
      }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  std::wstring input_desktop_name() {
    HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desktop) {
      return {};
    }

    WCHAR name[256] {};
    DWORD needed = 0;
    std::wstring result;
    if (GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
      result.assign(name);
    }

    CloseDesktop(desktop);
    return result;
  }

  BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, const LPARAM context) {
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
      return TRUE;
    }

    auto *out = reinterpret_cast<std::vector<std::pair<std::wstring, ti::rect_t>> *>(context);
    out->emplace_back(
      std::wstring {info.szDevice},
      ti::rect_t {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom}
    );
    return TRUE;
  }

  /**
   * @brief Rectangles of every monitor, and of the ones backed by a virtual display.
   * @details Slow — it enumerates display devices — so it never runs on the input path.
   */
  void enumerate_desktop_rects(std::vector<ti::rect_t> &all, std::vector<ti::rect_t> &virtual_displays) {
    std::vector<std::pair<std::wstring, ti::rect_t>> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&monitors));

    for (const auto &[device, rect] : monitors) {
      all.push_back(rect);
    }

    if (!VDISPLAY::isVirtualDisplayDriverInstalled()) {
      return;
    }

    for (const auto &info : VDISPLAY::enumerateVirtualDisplays()) {
      if (!info.is_active || info.device_name.empty()) {
        continue;
      }
      for (const auto &[device, rect] : monitors) {
        if (_wcsicmp(device.c_str(), info.device_name.c_str()) == 0) {
          virtual_displays.push_back(rect);
          break;
        }
      }
    }
  }

  void publish(const std::size_t slot, const ti::rect_t &rect) {
    g_published_lt[slot].store(pack(rect.left, rect.top), std::memory_order_relaxed);
    g_published_rb[slot].store(pack(rect.right, rect.bottom), std::memory_order_relaxed);
    g_published[slot].store(true, std::memory_order_release);
  }

  void unpublish(const std::size_t slot) {
    g_published[slot].store(false, std::memory_order_release);
  }

  void uninstall_hook() {
    if (g_timer_id) {
      KillTimer(nullptr, g_timer_id);
      g_timer_id = 0;
    }
    if (g_hook) {
      UnhookWindowsHookEx(g_hook);
      g_hook = nullptr;
      BOOST_LOG(info) << "Touch cursor isolation disengaged after swallowing "sv
                      << g_swallowed.load(std::memory_order_relaxed) << " promoted events."sv;
    }
    g_hook_desktop_name.clear();
  }

  void install_hook() {
    // A low-level hook only sees input on the desktop its thread is attached to.
    platf::syncThreadDesktop();
    g_hook_desktop_name = input_desktop_name();

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, hook_proc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
      BOOST_LOG(warning) << "Failed to install touch cursor isolation hook: "sv << GetLastError()
                         << " (running as SYSTEM: "sv << platf::is_running_as_system() << ")."sv;
      return;
    }

    if (!g_timer_id) {
      g_timer_id = SetTimer(nullptr, 0, kWatchdogIntervalMs, nullptr);
    }

    BOOST_LOG(info) << "Touch cursor isolation engaged on desktop '"sv
                    << platf::to_utf8(g_hook_desktop_name) << "'."sv;
  }

  /**
   * @brief Bring published state and the hook in line with the requested slots.
   * @details Recomputes everything from scratch, so it is safe to call at any time and
   * as often as needed.
   */
  void resolve_slots() {
    slot_t slots[MAX_SLOTS];
    {
      std::lock_guard<std::mutex> lock(state_mutex());
      std::copy(std::begin(g_slots), std::end(g_slots), std::begin(slots));
    }

    const bool wanted = std::any_of(std::begin(slots), std::end(slots), [](const slot_t &slot) {
      return slot.used;
    });

    std::vector<ti::rect_t> all;
    std::vector<ti::rect_t> virtual_displays;
    if (wanted) {
      enumerate_desktop_rects(all, virtual_displays);
    }

    bool any = false;
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
      std::optional<ti::rect_t> resolved;
      if (slots[i].used) {
        resolved = ti::match_virtual_display_rect(slots[i].requested, virtual_displays, all);
      }

      const bool was_published = g_published[i].load(std::memory_order_relaxed);

      if (!resolved) {
        if (slots[i].used && !was_published) {
          BOOST_LOG(debug) << "Not isolating ("sv << slots[i].requested.left << ','
                           << slots[i].requested.top << ")-("sv << slots[i].requested.right << ','
                           << slots[i].requested.bottom << "): no virtual display matches it exclusively."sv;
        }
        unpublish(i);
        continue;
      }

      if (!was_published) {
        BOOST_LOG(info) << "Isolating touch input within ("sv << resolved->left << ',' << resolved->top
                        << ")-("sv << resolved->right << ',' << resolved->bottom << ")."sv;
      }
      publish(i, *resolved);
      any = true;
    }

    g_any_published.store(any, std::memory_order_release);

    if (any && !g_hook) {
      install_hook();
    } else if (!any && g_hook) {
      uninstall_hook();
    }
  }

  /**
   * @brief Reattach the hook when the input desktop has changed.
   */
  void watchdog_tick() {
    if (!g_hook) {
      return;
    }

    const auto desktop = input_desktop_name();
    if (desktop.empty() || desktop == g_hook_desktop_name) {
      return;
    }

    BOOST_LOG(info) << "Input desktop changed to '"sv << platf::to_utf8(desktop)
                    << "'; reinstalling the touch cursor isolation hook."sv;
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    install_hook();
  }

  void thread_main() {
    MSG msg;
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    {
      std::lock_guard<std::mutex> lock(state_mutex());
      g_thread_id = GetCurrentThreadId();
    }

    // Picks up whatever was requested before the id above became visible, so a request
    // racing the thread start is never lost.
    resolve_slots();

    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == kMsgResolve) {
        resolve_slots();
      } else if (msg.message == WM_TIMER && msg.wParam == g_timer_id) {
        watchdog_tick();
      }
    }

    uninstall_hook();

    std::lock_guard<std::mutex> lock(state_mutex());
    g_thread_started = false;
    g_thread_id = 0;
  }

  /**
   * @brief Ask the isolation thread to re-resolve, starting it if needed.
   * @details Must be called with the state mutex held. It never blocks: this runs on
   * the input path, and a freshly started thread resolves on its own before entering
   * its message loop, so there is nothing to wait for.
   */
  void request_resolve_locked() {
    if (!g_thread_started) {
      g_thread_started = true;
      std::thread(thread_main).detach();
      return;
    }

    // Zero means the thread is starting up and has not published its id yet; its
    // initial resolve will see whatever we just wrote.
    if (g_thread_id != 0 && !PostThreadMessage(g_thread_id, kMsgResolve, 0, 0)) {
      BOOST_LOG(warning) << "Failed to notify the touch cursor isolation thread: "sv << GetLastError();
    }
  }
}  // namespace

namespace platf::touch_isolation {
  scope_t::scope_t(const std::size_t slot):
      _slot {slot} {
  }

  scope_t::~scope_t() {
    std::lock_guard<std::mutex> lock(state_mutex());
    g_slots[_slot] = {};
    request_resolve_locked();
  }

  std::shared_ptr<scope_t> acquire(const rect_t &streamed_rect) {
    if (!config::input.touch_cursor_isolation || is_empty(streamed_rect)) {
      return nullptr;
    }

    std::lock_guard<std::mutex> lock(state_mutex());

    std::size_t slot = MAX_SLOTS;
    for (std::size_t i = 0; i < MAX_SLOTS; ++i) {
      if (!g_slots[i].used) {
        slot = i;
        break;
      }
    }

    if (slot == MAX_SLOTS) {
      BOOST_LOG(warning) << "No free touch cursor isolation slot; the desktop cursor may follow touch input."sv;
      return nullptr;
    }

    g_slots[slot] = {true, streamed_rect};
    request_resolve_locked();

    return std::make_shared<scope_t>(slot);
  }

  void poke_cursor_visibility() {
    static std::atomic<std::int64_t> last_poke {0};

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
    )
                       .count();
    if (now - last_poke.load(std::memory_order_relaxed) < CURSOR_POKE_INTERVAL.count()) {
      return;
    }
    last_poke.store(now, std::memory_order_relaxed);

    CURSORINFO cursor {};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SUPPRESSED)) {
      return;
    }

    // A zero-delta relative move takes the pointer stack out of touch mode without
    // displacing the cursor. It carries no extra info, so our own hook ignores it.
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &input, sizeof(input)) != 1) {
      syncThreadDesktop();
      SendInput(1, &input, sizeof(input));
    }

    BOOST_LOG(debug) << "Restored the host cursor after Windows suppressed it for touch."sv;
  }
}  // namespace platf::touch_isolation
#endif

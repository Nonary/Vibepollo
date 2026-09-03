/**
 * @file src/platform/windows/local_input_monitor.cpp
 * @brief See local_input_monitor.h for the design rationale.
 */
#ifdef _WIN32

  #include "local_input_monitor.h"

  #include "src/logging.h"
  #include "src/rtsp.h"

  #include <atomic>
  #include <chrono>
  #include <mutex>
  #include <thread>

  #include <windows.h>
  #include <xinput.h>

  #ifdef _MSC_VER
    // MinGW links against xinput9_1_0 via CMake (windows.cmake); MSVC builds
    // need the equivalent pulled in here.
    #pragma comment(lib, "xinput9_1_0.lib")
  #endif

namespace local_input_monitor {

  namespace {

    using clock = std::chrono::steady_clock;
    constexpr auto CONTROLLER_POLL_INTERVAL = std::chrono::milliseconds(150);

    // Shared state. Written from the hook-thread's message loop (mouse/kb)
    // and from the controller poll loop, read from any thread via the
    // public API below.
    std::atomic<bool> g_running {false};
    std::atomic<clock::time_point> g_last_physical_input {clock::time_point::min()};

    void
      mark_physical_input() {
      g_last_physical_input.store(clock::now(), std::memory_order_relaxed);
    }

    // -- Low-level mouse/keyboard hooks --------------------------------------
    //
    // GetLastInputInfo cannot tell physical input apart from input injected
    // via SendInput, and Sunshine's own Windows input backend injects all
    // remote mouse/keyboard via SendInput (see platform/windows/input.cpp).
    // The low-level hooks are used specifically because they expose an
    // "injected" flag per event that GetLastInputInfo does not.

    LRESULT CALLBACK
      low_level_mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
      if (code == HC_ACTION) {
        const auto *info = reinterpret_cast<const MSLLHOOKSTRUCT *>(lparam);
        constexpr DWORD injected_mask = LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED;
        if ((info->flags & injected_mask) == 0) {
          mark_physical_input();
        }
      }
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    LRESULT CALLBACK
      low_level_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
      if (code == HC_ACTION) {
        const auto *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam);
        constexpr DWORD injected_mask = LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED;
        if ((info->flags & injected_mask) == 0) {
          mark_physical_input();
        }
      }
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // -- Controller (XInput) polling -----------------------------------------
    //
    // There is no per-event "injected" flag exposed to user-mode XInput
    // polling code, unlike the keyboard/mouse hooks above. ViGEm virtual
    // pads occupy their own XInput user index rather than merging into a
    // physical controller's slot, so in principle activity on a slot that
    // isn't ViGEm's is trustworthy - but Sunshine has no supported way to
    // learn which slot index ViGEm was assigned (that's decided by the OS at
    // attach time). So: poll_controllers_once() below only lets controller
    // activity count as physical input while rtsp_stream::session_count()
    // is zero, i.e. no Apollo-managed virtual pad could currently be
    // attached. Callers of locally_active()/time_since_local_input() get
    // this applied transparently; there's no separate trust flag to check.
    XINPUT_STATE g_last_pad_state[XUSER_MAX_COUNT] {};
    bool g_have_pad_baseline[XUSER_MAX_COUNT] {};

    void
      poll_controllers_once() {
      // A ViGEm virtual pad from an already-active session would also show
      // up as packet-number churn on some XInput slot, and there is no
      // injected-flag equivalent to filter it the way the keyboard/mouse
      // hooks do. So: only let controller activity count as "physical" when
      // nothing could currently be occupying a slot with virtual input. This
      // check has to happen per-poll (not just once at start) since a
      // session can start or end at any time while this loop is running.
      if (rtsp_stream::session_count() != 0) {
        // Still refresh the baseline so we don't get a false "activity"
        // packet-number jump the moment the session ends and trust resumes.
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
          XINPUT_STATE state {};
          if (XInputGetState(i, &state) == ERROR_SUCCESS) {
            g_last_pad_state[i] = state;
            g_have_pad_baseline[i] = true;
          } else {
            g_have_pad_baseline[i] = false;
          }
        }
        return;
      }

      for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state {};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) {
          g_have_pad_baseline[i] = false;
          continue;
        }
        if (g_have_pad_baseline[i] &&
            state.dwPacketNumber != g_last_pad_state[i].dwPacketNumber) {
          mark_physical_input();
        }
        g_last_pad_state[i] = state;
        g_have_pad_baseline[i] = true;
      }
    }

    // -- Hook-thread message loop --------------------------------------------

    std::thread g_hook_thread;
    std::atomic<DWORD> g_hook_thread_id {0};
    std::atomic<bool> g_stop {false};

    void
      hook_thread_main() {
      g_hook_thread_id.store(GetCurrentThreadId(), std::memory_order_release);

      HHOOK mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, low_level_mouse_proc, GetModuleHandleW(nullptr), 0);
      if (!mouse_hook) {
        BOOST_LOG(warning) << "local_input_monitor: failed to install WH_MOUSE_LL hook (err=" << GetLastError() << ")";
      }
      HHOOK keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard_proc, GetModuleHandleW(nullptr), 0);
      if (!keyboard_hook) {
        BOOST_LOG(warning) << "local_input_monitor: failed to install WH_KEYBOARD_LL hook (err=" << GetLastError() << ")";
      }

      // Low-level hooks require a message loop on the installing thread.
      // We piggyback controller polling onto the same loop via a timer so
      // this stays a single thread rather than spawning a second one.
      constexpr UINT_PTR CONTROLLER_TIMER_ID = 1;
      SetTimer(nullptr, CONTROLLER_TIMER_ID, static_cast<UINT>(CONTROLLER_POLL_INTERVAL.count()), nullptr);

      MSG msg;
      while (!g_stop.load(std::memory_order_acquire)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
          // WM_QUIT (0) or error (-1) - either way, stop.
          break;
        }
        if (msg.message == WM_TIMER && msg.wParam == CONTROLLER_TIMER_ID) {
          poll_controllers_once();
          continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      KillTimer(nullptr, CONTROLLER_TIMER_ID);
      if (mouse_hook) {
        UnhookWindowsHookEx(mouse_hook);
      }
      if (keyboard_hook) {
        UnhookWindowsHookEx(keyboard_hook);
      }
    }

    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
        if (!g_running.exchange(false, std::memory_order_acq_rel)) {
          return;
        }
        g_stop.store(true, std::memory_order_release);
        const DWORD tid = g_hook_thread_id.load(std::memory_order_acquire);
        if (tid != 0) {
          // Low-level hooks pump via GetMessage, which a plain condition
          // variable / atomic flag can't unblock - post WM_QUIT directly at
          // the thread that owns the message loop.
          PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        if (g_hook_thread.joinable()) {
          g_hook_thread.join();
        }
        BOOST_LOG(info) << "local_input_monitor: stopped";
      }
    };

  }  // namespace

  std::unique_ptr<platf::deinit_t>
    start() {
    if (g_running.exchange(true, std::memory_order_acq_rel)) {
      BOOST_LOG(warning) << "local_input_monitor: start() called while already running";
      return std::make_unique<deinit_t>();
    }
    g_stop.store(false, std::memory_order_release);
    g_last_physical_input.store(clock::time_point::min(), std::memory_order_relaxed);
    for (auto &has_baseline : g_have_pad_baseline) {
      has_baseline = false;
    }
    g_hook_thread = std::thread(hook_thread_main);
    BOOST_LOG(info) << "local_input_monitor: started";
    return std::make_unique<deinit_t>();
  }

  std::chrono::milliseconds
    time_since_local_input() {
    const auto last = g_last_physical_input.load(std::memory_order_relaxed);
    if (last == clock::time_point::min()) {
      return std::chrono::milliseconds::max();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - last);
  }

  bool
    locally_active(std::chrono::milliseconds threshold) {
    return time_since_local_input() <= threshold;
  }

}  // namespace local_input_monitor

#endif  // _WIN32

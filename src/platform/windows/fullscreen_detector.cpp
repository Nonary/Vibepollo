/**
 * @file src/platform/windows/fullscreen_detector.cpp
 */

#include "fullscreen_detector.h"

#include "src/logging.h"

#include <atomic>
#include <chrono>
#include <dwmapi.h>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <wtsapi32.h>

using namespace std::chrono_literals;

namespace platf::fullscreen_detector {
  namespace {
    constexpr LONG EDGE_TOLERANCE = 2;
    constexpr auto SHELL_CACHE_LIFETIME = 5s;

    bool rect_covers_capture_display(const RECT &window_rect, const RECT &capture_rect) {
      return window_rect.left <= capture_rect.left + EDGE_TOLERANCE &&
             window_rect.top <= capture_rect.top + EDGE_TOLERANCE &&
             window_rect.right >= capture_rect.right - EDGE_TOLERANCE &&
             window_rect.bottom >= capture_rect.bottom - EDGE_TOLERANCE;
    }

    bool window_covers_capture_display(HWND hwnd, const RECT &capture_rect) {
      if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
      }

      DWORD cloaked = 0;
      if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
          cloaked != 0) {
        return false;
      }

      RECT window_rect {};
      if (FAILED(DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &window_rect,
            sizeof(window_rect)
          )) &&
          !GetWindowRect(hwnd, &window_rect)) {
        return false;
      }

      return rect_covers_capture_display(window_rect, capture_rect);
    }

    bool obvious_passive_overlay(HWND hwnd) {
      if (!hwnd) {
        return false;
      }
      const auto ex_style = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      if ((ex_style & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) != 0) {
        return true;
      }
      return (ex_style & (WS_EX_LAYERED | WS_EX_TOOLWINDOW)) ==
             (WS_EX_LAYERED | WS_EX_TOOLWINDOW);
    }

    class shell_hook_monitor_t {
    public:
      shell_hook_monitor_t():
          worker_ {[this](std::stop_token stop_token) {
            run(stop_token);
          }} {
      }

      ~shell_hook_monitor_t() {
        worker_.request_stop();
        if (const auto hwnd = window_.load(std::memory_order_acquire)) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (worker_.joinable()) {
          worker_.join();
        }
      }

      shell_hook_monitor_t(const shell_hook_monitor_t &) = delete;
      shell_hook_monitor_t &operator=(const shell_hook_monitor_t &) = delete;

      result_t sample(const RECT &capture_rect) {
        if (!ready_.load(std::memory_order_acquire)) {
          return {};
        }

        HWND candidate {};
        DWORD pid = 0;
        {
          std::scoped_lock lock {mutex_};
          if (std::chrono::steady_clock::now() >= rude_window_deadline_) {
            clear_rude_window();
            return {};
          }
          candidate = rude_window_;
          pid = rude_window_pid_;
          DWORD current_pid = 0;
          if (!candidate ||
              !GetWindowThreadProcessId(candidate, &current_pid) ||
              current_pid == 0 ||
              current_pid != pid) {
            clear_rude_window();
            return {};
          }
        }
        if (!window_covers_capture_display(candidate, capture_rect)) {
          return {};
        }

        // Reject an HWND that was destroyed and reused after the snapshot above.
        DWORD current_pid = 0;
        if (!GetWindowThreadProcessId(candidate, &current_pid) ||
            current_pid == 0 ||
            current_pid != pid) {
          return {};
        }
        return {
          .verdict = verdict_e::fullscreen,
          .source = source_e::shell_hook,
          .pid = pid,
        };
      }

    private:
      static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == WM_NCCREATE) {
          const auto create = reinterpret_cast<CREATESTRUCTW *>(l_param);
          const auto self = static_cast<shell_hook_monitor_t *>(create->lpCreateParams);
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
          self->window_.store(hwnd, std::memory_order_release);
          return TRUE;
        }

        const auto self =
          reinterpret_cast<shell_hook_monitor_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) {
          return DefWindowProcW(hwnd, message, w_param, l_param);
        }

        if (message == self->shell_message_) {
          const auto activated = reinterpret_cast<HWND>(l_param);
          std::scoped_lock lock {self->mutex_};
          switch (w_param) {
            case HSHELL_RUDEAPPACTIVATED: {
              DWORD pid = 0;
              if (activated &&
                  GetWindowThreadProcessId(activated, &pid) &&
                  pid != 0) {
                self->rude_window_ = activated;
                self->rude_window_pid_ = pid;
                self->rude_window_deadline_ =
                  std::chrono::steady_clock::now() + SHELL_CACHE_LIFETIME;
              } else {
                self->clear_rude_window();
              }
              break;
            }
            case HSHELL_WINDOWACTIVATED:
              if (activated == self->rude_window_) {
                self->rude_window_deadline_ =
                  std::chrono::steady_clock::now() + SHELL_CACHE_LIFETIME;
                break;
              }
              // Non-activating and transparent tool windows are common overlay hosts.
              // Let them bridge a brief activation, but never indefinitely.
              if (!obvious_passive_overlay(activated)) {
                self->clear_rude_window();
              }
              break;
            case HSHELL_WINDOWDESTROYED:
              if (activated == self->rude_window_) {
                self->clear_rude_window();
              }
              break;
            default:
              break;
          }
          return 0;
        }

        switch (message) {
          case WM_WTSSESSION_CHANGE: {
            std::scoped_lock lock {self->mutex_};
            self->clear_rude_window();
            return 0;
          }
          case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
          case WM_DESTROY:
            WTSUnRegisterSessionNotification(hwnd);
            DeregisterShellHookWindow(hwnd);
            PostQuitMessage(0);
            return 0;
          case WM_NCDESTROY:
            self->window_.store(nullptr, std::memory_order_release);
            return DefWindowProcW(hwnd, message, w_param, l_param);
          default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
      }

      void clear_rude_window() {
        rude_window_ = nullptr;
        rude_window_pid_ = 0;
        rude_window_deadline_ = {};
      }

      void run(std::stop_token stop_token) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        const auto instance = GetModuleHandleW(nullptr);
        constexpr auto class_name = L"SunshineFullscreenDetectorWindow";
        WNDCLASSEXW window_class {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &shell_hook_monitor_t::window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name;

        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
          BOOST_LOG(warning) << "Fullscreen detector: failed to register Shell hook window class ["
                             << GetLastError() << ']';
          return;
        }

        const auto hwnd = CreateWindowExW(
          WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
          class_name,
          L"",
          WS_POPUP,
          0,
          0,
          0,
          0,
          nullptr,
          nullptr,
          instance,
          this
        );
        if (!hwnd) {
          window_.store(nullptr, std::memory_order_release);
          BOOST_LOG(warning) << "Fullscreen detector: failed to create Shell hook window ["
                             << GetLastError() << ']';
          return;
        }

        shell_message_ = RegisterWindowMessageW(L"SHELLHOOK");
        if (shell_message_ &&
            !ChangeWindowMessageFilterEx(hwnd, shell_message_, MSGFLT_ALLOW, nullptr)) {
          BOOST_LOG(warning) << "Fullscreen detector: failed to allow Shell hook message ["
                             << GetLastError() << ']';
        }
        if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
          BOOST_LOG(warning) << "Fullscreen detector: failed to register session notifications ["
                             << GetLastError() << ']';
        }
        if (!shell_message_ || !RegisterShellHookWindow(hwnd)) {
          BOOST_LOG(warning) << "Fullscreen detector: Shell hook unavailable ["
                             << GetLastError() << "]; continuing with remaining providers";
        } else {
          ready_.store(true, std::memory_order_release);
          BOOST_LOG(debug) << "Fullscreen detector: Shell hook provider ready";
        }

        if (stop_token.stop_requested()) {
          DestroyWindow(hwnd);
          window_.store(nullptr, std::memory_order_release);
          return;
        }

        MSG message {};
        while (!stop_token.stop_requested()) {
          const auto result = GetMessageW(&message, nullptr, 0, 0);
          if (result <= 0) {
            break;
          }
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }

        ready_.store(false, std::memory_order_release);
        if (IsWindow(hwnd)) {
          DestroyWindow(hwnd);
        }
        window_.store(nullptr, std::memory_order_release);
      }

      std::atomic<HWND> window_ {};
      std::atomic<bool> ready_ {false};
      UINT shell_message_ {0};
      std::mutex mutex_;
      HWND rude_window_ {};
      DWORD rude_window_pid_ {0};
      std::chrono::steady_clock::time_point rude_window_deadline_ {};
      std::jthread worker_;
    };

    shell_hook_monitor_t &shell_hook_monitor() {
      static shell_hook_monitor_t monitor;
      return monitor;
    }

    result_t exact_notification_state() {
      QUERY_USER_NOTIFICATION_STATE state {};
      if (FAILED(SHQueryUserNotificationState(&state))) {
        return {};
      }

      // Borderless games are intentionally handled by the other providers.
      // The remaining notification states are ambiguous (presentation mode,
      // quiet time, Store app, or merely accepting notifications), so only
      // the exact exclusive-D3D answer is authoritative here.
      if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
        return {
          .verdict = verdict_e::fullscreen,
          .source = source_e::notification_state,
        };
      }
      if (state == QUNS_NOT_PRESENT) {
        return {
          .verdict = verdict_e::desktop,
          .source = source_e::notification_state,
        };
      }
      return {};
    }
  }  // namespace

  result_t detect(const foreground_app::state_t &foreground, const RECT &capture_rect) {
    const bool attributed_game_window =
      foreground.source == "playnite-visible" ||
      foreground.source == "process-visible" ||
      (foreground.source == "fullscreen-visible" &&
       foreground.tracks_active_app_window);

    const auto notification = exact_notification_state();

    // A missing interactive session precludes a visible game on this display.
    if (notification.verdict == verdict_e::desktop) {
      return notification;
    }

    // Sample the remaining providers once, then evaluate the four fullscreen
    // strategies in order. Only a positive fullscreen result short-circuits;
    // negative evidence is retained for the final verdict after every provider
    // has had a chance to recognize the visible surface.
    const auto shell = shell_hook_monitor().sample(capture_rect);

    // Provider 1: a matching full-monitor game can remain composed beneath another
    // top-level window. Normal framed applications and definite desktop
    // surfaces make this provider decline; borderless, passive, and small
    // popups preserve the positive result without executable or vendor allowlists.
    const bool tracked_fullscreen_window =
      attributed_game_window &&
      foreground.matches_active_app &&
      foreground.fullscreen_on_capture_display;
    constexpr double SMALL_OVERLAY_COVERAGE_PERCENT = 15.0;
    const bool tracked_window_definitely_blocked =
      tracked_fullscreen_window &&
      foreground.blocker_present &&
      (foreground.definite_desktop_blocker_present ||
       (foreground.blocker_framed && !foreground.blocker_passive_overlay) ||
       (foreground.blocker_desktop_ui &&
        !foreground.blocker_passive_overlay &&
        foreground.blocker_coverage_percent > SMALL_OVERLAY_COVERAGE_PERCENT));
    if (tracked_fullscreen_window && !tracked_window_definitely_blocked) {
      if (foreground.blocker_present) {
        return {
          .verdict = verdict_e::fullscreen,
          .source = source_e::overlay_preserved,
          .pid = foreground.foreground_pid,
        };
      }
      return {
        .verdict = verdict_e::fullscreen,
        .source = source_e::tracked_window,
        .pid = foreground.foreground_pid,
      };
    }

    // Provider 2: Shell fullscreen activation is event-driven and display-scoped.
    // A different PID can be the game that a tracked launcher just handed off to.
    if (shell.verdict == verdict_e::fullscreen) {
      return shell;
    }

    // Provider 3: exact exclusive-D3D state is a session-wide positive signal.
    if (notification.verdict == verdict_e::fullscreen) {
      return notification;
    }

    // Provider 4: generic borderless/exclusive geometry is direct evidence on
    // this display. Active-app attribution can remain on a launcher when its
    // handoff signal is unavailable, so also inspect the opaque topmost blocker
    // that the attributed-window scan declined.
    if (foreground.source == "fullscreen-visible" &&
        foreground.valid_window &&
        foreground.fullscreen_on_capture_display) {
      return {
        .verdict = verdict_e::fullscreen,
        .source = source_e::borderless_window,
        .pid = foreground.foreground_pid,
      };
    }
    const bool generic_fullscreen_blocker =
      foreground.source == "desktop-visible" &&
      foreground.blocker_present &&
      foreground.blocker_opaque &&
      !foreground.blocker_framed &&
      !foreground.blocker_passive_overlay &&
      !foreground.blocker_desktop_ui &&
      rect_covers_capture_display(foreground.blocker_rect, capture_rect);
    if (generic_fullscreen_blocker) {
      return {
        .verdict = verdict_e::fullscreen,
        .source = source_e::borderless_window,
        .pid = foreground.blocker_pid,
      };
    }

    // All four fullscreen providers declined. Only now may direct desktop
    // evidence or the absence of the tracked game's full-monitor window demote.
    if (tracked_window_definitely_blocked) {
      return {
        .verdict = verdict_e::desktop,
        .source = source_e::desktop_window,
        .pid = foreground.definite_desktop_blocker_pid != 0 ?
                 foreground.definite_desktop_blocker_pid :
                 foreground.blocker_pid,
      };
    }
    if (foreground.source == "desktop-visible" &&
        (foreground.blocker_reason == "desktop-ui" || foreground.blocker_opaque)) {
      return {
        .verdict = verdict_e::desktop,
        .source = source_e::desktop_window,
        .pid = foreground.blocker_pid,
      };
    }
    if (foreground.tracks_active_app_window && !foreground.matching_game_fullscreen) {
      return {
        .verdict = verdict_e::desktop,
        .source = source_e::desktop_window,
        .pid = foreground.blocker_pid,
      };
    }

    return {};
  }

  const char *source_name(const source_e source) {
    switch (source) {
      case source_e::tracked_window:
        return "tracked-window";
      case source_e::shell_hook:
        return "shell-hook";
      case source_e::notification_state:
        return "notification-state";
      case source_e::borderless_window:
        return "borderless-window";
      case source_e::overlay_preserved:
        return "overlay-preserved";
      case source_e::desktop_window:
        return "desktop-window";
      default:
        return "none";
    }
  }

  const char *verdict_name(const verdict_e verdict) {
    switch (verdict) {
      case verdict_e::desktop:
        return "desktop";
      case verdict_e::fullscreen:
        return "fullscreen";
      default:
        return "unknown";
    }
  }

}  // namespace platf::fullscreen_detector

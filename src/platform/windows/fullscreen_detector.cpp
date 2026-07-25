/**
 * @file src/platform/windows/fullscreen_detector.cpp
 */

#include "fullscreen_detector.h"

#include "src/logging.h"

#include <atomic>
#include <dwmapi.h>
#include <mutex>
#include <shellapi.h>
#include <thread>

namespace platf::fullscreen_detector {
  namespace {
    constexpr LONG EDGE_TOLERANCE = 2;

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

      return window_rect.left <= capture_rect.left + EDGE_TOLERANCE &&
             window_rect.top <= capture_rect.top + EDGE_TOLERANCE &&
             window_rect.right >= capture_rect.right - EDGE_TOLERANCE &&
             window_rect.bottom >= capture_rect.bottom - EDGE_TOLERANCE;
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
        {
          std::scoped_lock lock {mutex_};
          candidate = rude_window_;
        }
        if (!window_covers_capture_display(candidate, capture_rect)) {
          return {};
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(candidate, &pid);
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
            case HSHELL_RUDEAPPACTIVATED:
              self->rude_window_ = activated;
              break;
            case HSHELL_WINDOWACTIVATED:
              // Non-activating and transparent tool windows are common overlay hosts.
              // Do not let one erase the last Shell-confirmed fullscreen window.
              if (!obvious_passive_overlay(activated)) {
                self->rude_window_ = nullptr;
              }
              break;
            case HSHELL_WINDOWDESTROYED:
              if (activated == self->rude_window_) {
                self->rude_window_ = nullptr;
              }
              break;
            default:
              break;
          }
          return 0;
        }

        switch (message) {
          case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
          case WM_DESTROY:
            DeregisterShellHookWindow(hwnd);
            PostQuitMessage(0);
            return 0;
          default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
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
          BOOST_LOG(warning) << "Fullscreen detector: failed to create Shell hook window ["
                             << GetLastError() << ']';
          return;
        }

        shell_message_ = RegisterWindowMessageW(L"SHELLHOOK");
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

      std::jthread worker_;
      std::atomic<HWND> window_ {};
      std::atomic<bool> ready_ {false};
      UINT shell_message_ {0};
      std::mutex mutex_;
      HWND rude_window_ {};
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
    // Provider 1: strongest answer, because it supplies both fullscreen geometry
    // and game identity from the launched process or Playnite.
    const bool attributed_game_window =
      foreground.source == "playnite-visible" ||
      foreground.source == "process-visible" ||
      foreground.source == "playnite-status" ||
      foreground.source == "playnite-cache" ||
      foreground.source == "process";
    if (attributed_game_window &&
        foreground.matches_active_app &&
        foreground.fullscreen_on_capture_display) {
      return {
        .verdict = verdict_e::fullscreen,
        .source = source_e::tracked_window,
        .pid = foreground.foreground_pid,
      };
    }

    // Provider 2: Shell fullscreen activation. It is event-driven, does not draw an
    // overlay, and does not inject a hook DLL into the game.
    if (auto shell = shell_hook_monitor().sample(capture_rect);
        shell.verdict != verdict_e::unknown) {
      return shell;
    }

    // Provider 3: exact exclusive-D3D notification state. Ambiguous notification
    // states deliberately fall through to borderless geometry.
    if (auto notification = exact_notification_state();
        notification.verdict != verdict_e::unknown) {
      return notification;
    }

    // Provider 4: generic borderless/exclusive geometry. At this point there was no
    // attributable game, but a full-monitor surface is still fullscreen policy-wise.
    const bool generic_fullscreen_window =
      foreground.source == "fullscreen-visible" ||
      foreground.source == "fullscreen-foreground" ||
      foreground.source == "playnite-fullscreen";
    if (generic_fullscreen_window &&
        foreground.valid_window &&
        foreground.fullscreen_on_capture_display) {
      return {
        .verdict = verdict_e::fullscreen,
        .source = source_e::borderless_window,
        .pid = foreground.foreground_pid,
      };
    }

    // None of the four fullscreen providers succeeded. A known Windows desktop
    // surface or a genuinely opaque unrelated window is now definitive. A
    // translucent/unmeasurable surface remains unknown instead.
    if (foreground.source == "desktop-visible" &&
        (foreground.blocker_reason == "desktop-ui" || foreground.blocker_opaque)) {
      return {
        .verdict = verdict_e::desktop,
        .source = source_e::desktop_window,
        .pid = foreground.blocker_pid,
      };
    }

    if (foreground.source == "desktop-visible") {
      return {
        .verdict = foreground.blocker_opaque ? verdict_e::desktop : verdict_e::unknown,
        .source = foreground.blocker_opaque ? source_e::desktop_window : source_e::none,
        .pid = foreground.blocker_pid,
      };
    }
    if (foreground.source == "visibility-unknown") {
      return {};
    }

    return {
      .verdict = verdict_e::desktop,
      .source = source_e::desktop_window,
      .pid = foreground.foreground_pid,
    };
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

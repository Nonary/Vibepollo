/**
 * @file src/platform/windows/foreground_app.cpp
 */

#include "foreground_app.h"
#include "rtx_hdr_policy.h"

#include "playnite_integration.h"
#include "src/process.h"
#include "tools/playnite_launcher/focus_utils.h"
#include "utf_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>

#include <dwmapi.h>

namespace platf::foreground_app {
  namespace {

    std::string lower_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    std::string normalize_path_text(std::string_view value) {
      if (value.empty()) {
        return {};
      }

      std::string text(value);
      std::replace(text.begin(), text.end(), '/', '\\');
      try {
        auto wide = utf_utils::from_utf8(text);
        auto path = std::filesystem::path(wide).lexically_normal();
        text = utf_utils::to_utf8(path.wstring());
      } catch (...) {
      }
      std::replace(text.begin(), text.end(), '/', '\\');
      while (!text.empty() && (text.back() == '\\' || text.back() == '/')) {
        text.pop_back();
      }
      return lower_ascii(text);
    }

    std::string basename_text(std::string_view value) {
      if (value.empty()) {
        return {};
      }
      try {
        auto wide = utf_utils::from_utf8(std::string(value));
        return lower_ascii(utf_utils::to_utf8(std::filesystem::path(wide).filename().wstring()));
      } catch (...) {
      }
      auto text = std::string(value);
      std::replace(text.begin(), text.end(), '/', '\\');
      auto pos = text.find_last_of('\\');
      if (pos != std::string::npos) {
        text = text.substr(pos + 1);
      }
      return lower_ascii(text);
    }

    bool foreground_window_is_windows_shell(HWND hwnd) {
      wchar_t class_name[256] {};
      if (!GetClassNameW(hwnd, class_name, static_cast<int>(sizeof(class_name) / sizeof(class_name[0])))) {
        return false;
      }

      return wcscmp(class_name, L"Progman") == 0 ||
             wcscmp(class_name, L"WorkerW") == 0 ||
             wcscmp(class_name, L"SHELLDLL_DefView") == 0 ||
             wcscmp(class_name, L"Shell_TrayWnd") == 0 ||
             wcscmp(class_name, L"Shell_SecondaryTrayWnd") == 0;
    }

    std::string window_class_utf8(HWND hwnd) {
      wchar_t class_name[256] {};
      if (!GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)))) {
        return {};
      }
      try {
        return utf_utils::to_utf8(class_name);
      } catch (...) {
        return {};
      }
    }

    std::string window_title_utf8(HWND hwnd) {
      const auto length = GetWindowTextLengthW(hwnd);
      if (length <= 0) {
        return {};
      }
      std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
      const auto copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
      if (copied <= 0) {
        return {};
      }
      title.resize(static_cast<std::size_t>(copied));
      try {
        return utf_utils::to_utf8(title);
      } catch (...) {
        return {};
      }
    }

    bool window_is_passive_compositor_host(HWND hwnd) {
      const auto style = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE));
      const auto ex_style = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      if (game_activity_policy::passive_compositor_style(style, ex_style)) {
        return true;
      }
      if ((ex_style & WS_EX_LAYERED) != 0) {
        COLORREF color_key = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &flags)) {
          return (flags & LWA_COLORKEY) != 0 ||
                 ((flags & LWA_ALPHA) != 0 && alpha < 255);
        }

      }
      return false;
    }

    bool foreground_window_is_fullscreen_on_capture_display(HWND hwnd, const RECT &capture_rect) {
      if (!hwnd || hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) {
        return false;
      }
      if (foreground_window_is_windows_shell(hwnd)) {
        return false;
      }
      if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
      }

      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      if ((style & (WS_CAPTION | WS_THICKFRAME)) != 0) {
        return false;
      }

      RECT window_rect {};
      if (!GetWindowRect(hwnd, &window_rect)) {
        return false;
      }

      RECT intersection {};
      if (!IntersectRect(&intersection, &window_rect, &capture_rect)) {
        return false;
      }

      const auto capture_area = static_cast<long long>(capture_rect.right - capture_rect.left) *
                                static_cast<long long>(capture_rect.bottom - capture_rect.top);
      const auto intersection_area = static_cast<long long>(intersection.right - intersection.left) *
                                     static_cast<long long>(intersection.bottom - intersection.top);
      return capture_area > 0 && intersection_area * 100 >= capture_area * 90;
    }

    std::string process_image_path_utf8(DWORD pid) {
      std::wstring path;
      if (!playnite_launcher::focus::get_process_image_path(pid, path)) {
        return {};
      }
      try {
        return utf_utils::to_utf8(path);
      } catch (...) {
        return {};
      }
    }

    using visible_stack_decision_e = game_activity_policy::visible_stack_decision_e;

    // Shell furniture that floats above a fullscreen game during an alt-tab is not
    // evidence that the game stopped being fullscreen. If the desktop really is
    // exposed, the window underneath (Progman/WorkerW, an explorer window, or the
    // app the user switched to) still blocks, so looking past these costs nothing.
    bool window_is_transient_shell_overlay(
      const std::string_view class_name,
      const bool desktop_ui,
      const bool covers_capture_display
    ) {
      if (!desktop_ui || class_name.empty()) {
        return false;
      }
      // Alt-tab switcher, Task View, snap assist, and the legacy switchers.
      if (class_name == "XamlExplorerHostIslandWindow" ||
          class_name == "MultitaskingViewFrame" ||
          class_name == "TaskSwitcherWnd" ||
          class_name == "TaskSwitcherOverlayWnd" ||
          class_name == "ForegroundStaging") {
        return true;
      }
      // A taskbar that covers a strip of the display, not the display itself.
      return !covers_capture_display &&
             (class_name == "Shell_TrayWnd" || class_name == "Shell_SecondaryTrayWnd");
    }

    [[maybe_unused]] visible_stack_decision_e evaluate_visible_window(
      const visible_window_evidence_t &evidence,
      const bool require_active_app_match
    ) {
      return game_activity_policy::evaluate_visible_window(evidence, require_active_app_match);
    }

    bool window_is_cloaked(HWND hwnd) {
      DWORD cloaked = 0;
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
             cloaked != 0;
    }

    bool window_has_visible_alpha(HWND hwnd) {
      const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      if ((ex_style & WS_EX_LAYERED) == 0) {
        return true;
      }

      COLORREF color_key = 0;
      BYTE alpha = 255;
      DWORD flags = 0;
      if (!GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &flags)) {
        return true;
      }
      return (flags & LWA_ALPHA) == 0 || alpha != 0;
    }

    bool window_is_opaque(HWND hwnd) {
      const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      if ((ex_style & WS_EX_LAYERED) == 0) {
        return true;
      }

      COLORREF color_key = 0;
      BYTE alpha = 255;
      DWORD flags = 0;
      if (!GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &flags)) {
        return false;
      }
      if ((flags & LWA_COLORKEY) != 0) {
        return false;
      }
      return (flags & LWA_ALPHA) != 0 && alpha == 255;
    }

    std::optional<RECT> visible_window_rect(HWND hwnd, const RECT &capture_rect) {
      if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || window_is_cloaked(hwnd) ||
          !window_has_visible_alpha(hwnd)) {
        return std::nullopt;
      }

      RECT window_rect {};
      if (FAILED(DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &window_rect,
            sizeof(window_rect)
          )) &&
          !GetWindowRect(hwnd, &window_rect)) {
        return std::nullopt;
      }

      RECT intersection {};
      if (!IntersectRect(&intersection, &window_rect, &capture_rect)) {
        return std::nullopt;
      }

      // Ignore DWM/input-sink slivers and the hidden edge of an auto-hidden taskbar.
      // Actual taskbars, menus, notifications, and application windows are much larger.
      if (intersection.right - intersection.left < 3 ||
          intersection.bottom - intersection.top < 3) {
        return std::nullopt;
      }
      return window_rect;
    }

    bool window_fully_covers_capture_display(HWND hwnd, const RECT &window_rect, const RECT &capture_rect) {
      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      if ((style & (WS_CAPTION | WS_THICKFRAME)) != 0) {
        return false;
      }

      // Allow only a tiny DWM rounding/border tolerance. The older 90%-coverage
      // heuristic is intentionally too permissive here because it can leave a
      // visibly exposed desktop strip while treating the game as fullscreen.
      constexpr LONG EDGE_TOLERANCE = 2;
      return window_rect.left <= capture_rect.left + EDGE_TOLERANCE &&
             window_rect.top <= capture_rect.top + EDGE_TOLERANCE &&
             window_rect.right >= capture_rect.right - EDGE_TOLERANCE &&
             window_rect.bottom >= capture_rect.bottom - EDGE_TOLERANCE;
    }

    double window_coverage_percent(const RECT &window_rect, const RECT &capture_rect) {
      RECT intersection {};
      if (!IntersectRect(&intersection, &window_rect, &capture_rect)) {
        return 0;
      }
      const auto capture_width = static_cast<long long>(capture_rect.right) - capture_rect.left;
      const auto capture_height = static_cast<long long>(capture_rect.bottom) - capture_rect.top;
      const auto intersection_width = static_cast<long long>(intersection.right) - intersection.left;
      const auto intersection_height = static_cast<long long>(intersection.bottom) - intersection.top;
      const auto capture_area = capture_width * capture_height;
      const auto intersection_area = intersection_width * intersection_height;
      if (capture_area <= 0 || intersection_area <= 0) {
        return 0;
      }
      return std::min(100.0, static_cast<double>(intersection_area) * 100.0 / static_cast<double>(capture_area));
    }

    bool process_is_windows_desktop_ui(std::string_view executable) {
      const auto basename = basename_text(executable);
      return basename == "explorer.exe" ||
             basename == "startmenuexperiencehost.exe" ||
             basename == "searchhost.exe" ||
             basename == "searchapp.exe" ||
             basename == "shellexperiencehost.exe" ||
             basename == "textinputhost.exe" ||
             basename == "lockapp.exe" ||
             basename == "systemsettings.exe" ||
             basename == "applicationframehost.exe";
    }

    struct visible_window_t {
      HWND hwnd {};
      DWORD pid {};
      RECT rect {};
      double coverage_percent {0.0};
      std::string executable;
      std::string class_name;
      std::string title;
      visible_window_evidence_t evidence;
      bool framed {false};
    };

    struct visible_window_result_t {
      std::optional<visible_window_t> selected;
      std::optional<visible_window_t> blocker;
      std::optional<visible_window_t> overlay;
      std::optional<visible_window_t> definite_desktop_blocker;
      bool matching_window_seen {false};
      std::uint32_t ignored_passive_window_count {0};
      bool blocked {false};
    };

    using active_window_matcher_t = std::function<bool(DWORD, std::string_view)>;

    visible_window_result_t find_top_visible_fullscreen_window(
      const RECT &capture_rect,
      const bool require_active_app_match,
      const active_window_matcher_t &matches_active_app
    ) {
      struct enum_context_t {
        const RECT &capture_rect;
        bool require_active_app_match;
        const active_window_matcher_t &matches_active_app;
        std::optional<visible_window_t> selected;
        std::optional<visible_window_t> blocker;
        std::optional<visible_window_t> overlay;
        std::optional<visible_window_t> definite_desktop_blocker;
        bool matching_window_seen {false};
        std::uint32_t ignored_passive_window_count {0};
        bool blocked {false};
      } context {
        capture_rect,
        require_active_app_match,
        matches_active_app,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        false,
        0,
        false,
      };

      EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto &context = *reinterpret_cast<enum_context_t *>(param);
        const auto window_rect = visible_window_rect(hwnd, context.capture_rect);
        if (!window_rect) {
          return TRUE;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        auto executable = process_image_path_utf8(pid);

        visible_window_t window;
        window.hwnd = hwnd;
        window.pid = pid;
        window.rect = *window_rect;
        window.coverage_percent =
          window_coverage_percent(*window_rect, context.capture_rect);
        window.executable = std::move(executable);
        window.class_name = window_class_utf8(hwnd);
        window.title = window_title_utf8(hwnd);
        window.evidence.belongs_to_active_app =
          context.require_active_app_match &&
          context.matches_active_app &&
          context.matches_active_app(window.pid, window.executable);
        window.evidence.desktop_ui =
          hwnd == GetDesktopWindow() ||
          hwnd == GetShellWindow() ||
          foreground_window_is_windows_shell(hwnd) ||
          process_is_windows_desktop_ui(window.executable) ||
          (!context.require_active_app_match && window.executable.empty());
        window.evidence.fullscreen_on_capture_display =
          window_fully_covers_capture_display(hwnd, *window_rect, context.capture_rect);
        window.evidence.transient_shell_overlay = window_is_transient_shell_overlay(
          window.class_name,
          window.evidence.desktop_ui,
          window.evidence.fullscreen_on_capture_display
        );
        window.evidence.opaque = window_is_opaque(hwnd);
        const auto style = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        window.framed = (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
        // DirectComposition/Electron/CEF overlays commonly keep transparent
        // top-level hosts above the game. Win32 reports these hosts as visible
        // even when they paint no pixels. Classify them by generic activation,
        // transparency, layering, and tool-window styles rather than by vendor.
        window.evidence.passive_host =
          !window.evidence.desktop_ui &&
          !window.evidence.belongs_to_active_app &&
          window_is_passive_compositor_host(hwnd);
        if (window.evidence.belongs_to_active_app) {
          context.matching_window_seen = true;
        }

        switch (game_activity_policy::evaluate_visible_window(window.evidence, context.require_active_app_match)) {
          case visible_stack_decision_e::continue_scan:
            if (window.evidence.passive_host || window.evidence.transient_shell_overlay ||
                !window.evidence.opaque) {
              ++context.ignored_passive_window_count;
              if (!window.evidence.belongs_to_active_app && !context.overlay) {
                context.overlay = window;
              }
            }
            return TRUE;
          case visible_stack_decision_e::select_game:
            context.selected = std::move(window);
            return FALSE;
          case visible_stack_decision_e::block:
            context.blocked = true;
            if (!context.definite_desktop_blocker &&
                (window.framed ||
                 (window.evidence.desktop_ui && window.coverage_percent > 15.0))) {
              context.definite_desktop_blocker = window;
            }
            if (!context.blocker) {
              context.blocker = std::move(window);
            }
            // With an attributed game, a window above it is composition
            // evidence, not the end of the scan. Keep the first blocker and
            // continue until the matching full-monitor game is found (or the
            // stack is exhausted).
            return context.require_active_app_match ? TRUE : FALSE;
        }
        return FALSE;
      },
                  reinterpret_cast<LPARAM>(&context));

      return {
        std::move(context.selected),
        std::move(context.blocker),
        std::move(context.overlay),
        std::move(context.definite_desktop_blocker),
        context.matching_window_seen,
        context.ignored_passive_window_count,
        context.blocked,
      };
    }

  }  // namespace

  bool path_equal_or_basename_match(std::string_view lhs, std::string_view rhs) {
    if (lhs.empty() || rhs.empty()) {
      return false;
    }
    const auto left = normalize_path_text(lhs);
    const auto right = normalize_path_text(rhs);
    if (!left.empty() && left == right) {
      return true;
    }
    const auto left_base = basename_text(lhs);
    const auto right_base = basename_text(rhs);
    return !left_base.empty() && left_base == right_base;
  }

  bool path_is_under_directory(std::string_view path, std::string_view directory) {
    const auto child = normalize_path_text(path);
    const auto parent = normalize_path_text(directory);
    if (child.empty() || parent.empty() || child.size() <= parent.size()) {
      return false;
    }
    if (child.compare(0, parent.size(), parent) != 0) {
      return false;
    }
    return child[parent.size()] == '\\';
  }

  bool playnite_foreground_matches_for_tests(
    std::string_view active_playnite_id,
    std::string_view status_id,
    std::string_view status_exe,
    std::string_view status_install_dir,
    std::string_view foreground_exe
  ) {
    return rtx_hdr::policy::playnite_foreground_matches(active_playnite_id, status_id, status_exe, status_install_dir, foreground_exe);
  }

  bool transient_shell_overlay_for_tests(
    const std::string_view class_name,
    const bool desktop_ui,
    const bool covers_capture_display
  ) {
    return window_is_transient_shell_overlay(class_name, desktop_ui, covers_capture_display);
  }

  state_t snapshot(
    const std::optional<RECT> &capture_rect,
    const DWORD game_hint_pid,
    const std::string_view game_hint_exe
  ) {
    state_t state;

    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
      state.shell_window =
        hwnd == GetDesktopWindow() ||
        hwnd == GetShellWindow() ||
        foreground_window_is_windows_shell(hwnd);
      state.valid_window =
        !state.shell_window &&
        IsWindowVisible(hwnd) &&
        !IsIconic(hwnd) &&
        !window_is_cloaked(hwnd) &&
        window_has_visible_alpha(hwnd);
      if (state.valid_window) {
        if (capture_rect) {
          state.fullscreen_on_capture_display =
            foreground_window_is_fullscreen_on_capture_display(hwnd, *capture_rect);
        }
        DWORD pid = 0;
        if (GetWindowThreadProcessId(hwnd, &pid) && pid != 0) {
          state.foreground_pid = pid;
          state.foreground_exe = process_image_path_utf8(pid);
        }
      }
    }

    const auto app = proc::proc.running_app_state();
    state.has_active_app = app.has_active_app;
    state.uses_playnite = app.uses_playnite;
    state.active_app_name = app.name;

    std::optional<platf::playnite::active_game_status_t> playnite_status;
    std::string cached_install_dir;
    if (app.uses_playnite) {
      const auto active_games = platf::playnite::get_active_game_statuses();
      for (auto game = active_games.rbegin(); game != active_games.rend(); ++game) {
        if (game->active && game->id == app.playnite_id) {
          playnite_status = *game;
          break;
        }
      }
      platf::playnite::get_cached_install_dir(app.playnite_id, cached_install_dir);
    }

    if (capture_rect) {
      bool require_active_app_match =
        state.has_active_app && (app.uses_playnite || app.trackable);

      active_window_matcher_t matcher;
      if (app.uses_playnite) {
        matcher = [playnite_status, cached_install_dir](DWORD, const std::string_view executable) {
          if (playnite_status &&
              playnite_foreground_matches_for_tests(
                {},
                playnite_status->id,
                playnite_status->exe,
                playnite_status->install_dir,
                executable
              )) {
            return true;
          }
          return !cached_install_dir.empty() &&
                 path_is_under_directory(executable, cached_install_dir);
        };
      } else if (app.trackable) {
        matcher = [](const DWORD pid, std::string_view) {
          return proc::proc.running_app_contains_pid(pid);
        };
      } else if (game_hint_pid != 0 || !game_hint_exe.empty()) {
        require_active_app_match = true;
        matcher = [game_hint_pid, game_hint_exe = std::string(game_hint_exe)](
                    const DWORD pid,
                    const std::string_view executable
                  ) {
          return (game_hint_pid != 0 && pid == game_hint_pid) ||
                 path_equal_or_basename_match(executable, game_hint_exe);
        };
      }
      state.tracks_active_app_window = require_active_app_match;

      const auto visible_result = find_top_visible_fullscreen_window(
        *capture_rect,
        require_active_app_match,
        matcher
      );
      state.ignored_passive_window_count =
        visible_result.ignored_passive_window_count;
      state.matching_window_seen = visible_result.matching_window_seen;
      state.matching_game_fullscreen = visible_result.selected.has_value();
      state.definite_desktop_blocker_present =
        visible_result.definite_desktop_blocker.has_value();
      if (visible_result.definite_desktop_blocker) {
        state.definite_desktop_blocker_pid =
          visible_result.definite_desktop_blocker->pid;
      }

      const auto populate_blocker = [&](const visible_window_t &blocker, const bool passive_overlay) {
        state.blocker_present = true;
        state.blocker_pid = blocker.pid;
        state.blocker_exe = blocker.executable;
        state.blocker_class = blocker.class_name;
        state.blocker_title = blocker.title;
        state.blocker_rect = blocker.rect;
        state.blocker_coverage_percent = blocker.coverage_percent;
        state.blocker_opaque = blocker.evidence.opaque;
        state.blocker_framed = blocker.framed;
        state.blocker_passive_overlay =
          passive_overlay || blocker.evidence.passive_host ||
          blocker.evidence.transient_shell_overlay || !blocker.evidence.opaque;
        state.blocker_desktop_ui = blocker.evidence.desktop_ui;

        constexpr double SMALL_OVERLAY_COVERAGE_PERCENT = 15.0;
        if (state.blocker_framed && !state.blocker_passive_overlay) {
          state.blocker_classification = "framed-application";
        } else if (state.blocker_desktop_ui &&
                   !state.blocker_passive_overlay &&
                   state.blocker_coverage_percent > SMALL_OVERLAY_COVERAGE_PERCENT) {
          state.blocker_classification = "desktop-ui";
        } else if (state.blocker_passive_overlay) {
          state.blocker_classification = "passive-overlay";
        } else if (state.blocker_coverage_percent <= SMALL_OVERLAY_COVERAGE_PERCENT) {
          state.blocker_classification = "small-popup";
        } else {
          state.blocker_classification = "borderless-overlay";
        }

        if (blocker.evidence.desktop_ui) {
          state.blocker_reason = "desktop-ui";
        } else if (passive_overlay) {
          state.blocker_reason = "passive-overlay";
        } else if (require_active_app_match && !blocker.evidence.belongs_to_active_app) {
          state.blocker_reason = "unrelated-window";
        } else {
          state.blocker_reason = "not-fullscreen";
        }
      };

      if (visible_result.blocker) {
        populate_blocker(*visible_result.blocker, false);
      } else if (visible_result.overlay) {
        populate_blocker(*visible_result.overlay, true);
      }

      if (visible_result.selected) {
        const auto &visible_game = *visible_result.selected;
        state.valid_window = true;
        state.shell_window = false;
        state.fullscreen_on_capture_display = true;
        state.matches_active_app = true;
        state.foreground_pid = visible_game.pid;
        state.foreground_exe = visible_game.executable;
        state.active_app_exe =
          playnite_status && !playnite_status->exe.empty() ?
            playnite_status->exe :
            visible_game.executable;
        if (app.uses_playnite) {
          state.source = "playnite-visible";
        } else if (app.trackable) {
          state.source = "process-visible";
        } else {
          state.source = "fullscreen-visible";
        }
        return state;
      }

      state.source = visible_result.blocked ? "desktop-visible" : "visibility-unknown";
      return state;
    }

    if (!state.has_active_app) {
      state.matches_active_app = state.fullscreen_on_capture_display;
      state.active_app_exe = state.foreground_exe;
      state.source = state.matches_active_app ? "fullscreen-foreground" : "none";
      return state;
    }

    if (app.uses_playnite) {
      if (playnite_status &&
          playnite_foreground_matches_for_tests(
            app.playnite_id,
            playnite_status->id,
            playnite_status->exe,
            playnite_status->install_dir,
            state.foreground_exe
          )) {
        state.matches_active_app = true;
        state.active_app_exe =
          !playnite_status->exe.empty() ?
            playnite_status->exe :
            state.foreground_exe;
        state.source = "playnite-status";
        return state;
      }

      if (!cached_install_dir.empty() &&
          path_is_under_directory(state.foreground_exe, cached_install_dir)) {
        state.matches_active_app = true;
        state.active_app_exe = state.foreground_exe;
        state.source = "playnite-cache";
        return state;
      }
    }

    if (state.foreground_pid != 0 && proc::proc.running_app_contains_pid(state.foreground_pid)) {
      state.matches_active_app = true;
      state.active_app_exe = state.foreground_exe;
      state.source = "process";
      return state;
    }

    if ((app.uses_playnite || !app.trackable) && state.fullscreen_on_capture_display) {
      state.matches_active_app = true;
      state.active_app_exe = state.foreground_exe;
      state.source = app.uses_playnite ? "playnite-fullscreen" : "fullscreen-foreground";
      return state;
    }

    state.source = "foreground-mismatch";
    return state;
  }

}  // namespace platf::foreground_app

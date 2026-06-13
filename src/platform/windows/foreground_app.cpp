/**
 * @file src/platform/windows/foreground_app.cpp
 */

#include "foreground_app.h"

#include "playnite_integration.h"
#include "src/process.h"
#include "tools/playnite_launcher/focus_utils.h"
#include "utf_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

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
    if (foreground_exe.empty() || status_id.empty()) {
      return false;
    }
    if (!active_playnite_id.empty() && active_playnite_id != status_id) {
      return false;
    }
    if (path_equal_or_basename_match(foreground_exe, status_exe)) {
      return true;
    }
    return path_is_under_directory(foreground_exe, status_install_dir);
  }

  state_t snapshot(const std::optional<RECT> &capture_rect) {
    state_t state;

    HWND hwnd = GetForegroundWindow();
    if (!hwnd || hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) {
      return state;
    }

    state.shell_window = foreground_window_is_windows_shell(hwnd);
    if (state.shell_window || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      return state;
    }

    state.valid_window = true;
    if (capture_rect) {
      state.fullscreen_on_capture_display = foreground_window_is_fullscreen_on_capture_display(hwnd, *capture_rect);
    }

    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) && pid != 0) {
      state.foreground_pid = pid;
      state.foreground_exe = process_image_path_utf8(pid);
    }

    const auto app = proc::proc.running_app_state();
    state.has_active_app = app.has_active_app;
    state.uses_playnite = app.uses_playnite;
    state.active_app_name = app.name;

    if (!state.has_active_app) {
      state.matches_active_app = state.fullscreen_on_capture_display;
      state.active_app_exe = state.foreground_exe;
      state.source = state.matches_active_app ? "fullscreen-foreground" : "none";
      return state;
    }

    if (app.uses_playnite) {
      const auto playnite_status = platf::playnite::get_active_game_status();
      if (playnite_status.active &&
          playnite_foreground_matches_for_tests(app.playnite_id, playnite_status.id, playnite_status.exe, playnite_status.install_dir, state.foreground_exe)) {
        state.matches_active_app = true;
        state.active_app_exe = !playnite_status.exe.empty() ? playnite_status.exe : state.foreground_exe;
        state.source = "playnite-status";
        return state;
      }

      std::string cached_install_dir;
      if (platf::playnite::get_cached_install_dir(app.playnite_id, cached_install_dir) &&
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

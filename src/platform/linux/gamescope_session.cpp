/**
 * @file src/platform/linux/gamescope_session.cpp
 * @brief Safe discovery of the Gamescope Wayland socket for the current user.
 */
#include "gamescope_session.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  constexpr std::size_t max_environment_size = 16 * 1024;

  std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
      value.remove_suffix(1);
    }
    return value;
  }

  std::optional<std::string> parse_value(std::string_view value) {
    value = trim(value);
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') || (value.front() == '"' && value.back() == '"'))) {
      value.remove_prefix(1);
      value.remove_suffix(1);
    }
    if (value.empty()) {
      return std::nullopt;
    }
    for (const unsigned char ch : value) {
      if (std::iscntrl(ch)) {
        return std::nullopt;
      }
    }
    return std::string {value};
  }

  std::optional<std::string> read_environment_file(const char *runtime_dir) {
    if (!runtime_dir || runtime_dir[0] != '/') {
      return std::nullopt;
    }

    const int dir_fd = open(runtime_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir_fd < 0) {
      return std::nullopt;
    }

    struct stat dir_stat {};
    if (fstat(dir_fd, &dir_stat) < 0 || !S_ISDIR(dir_stat.st_mode) || dir_stat.st_uid != geteuid()) {
      close(dir_fd);
      return std::nullopt;
    }

    const int file_fd = openat(dir_fd, "gamescope-environment", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(dir_fd);
    if (file_fd < 0) {
      return std::nullopt;
    }

    struct stat file_stat {};
    if (fstat(file_fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode) || file_stat.st_uid != geteuid() || file_stat.st_size < 0 || static_cast<std::size_t>(file_stat.st_size) > max_environment_size) {
      close(file_fd);
      return std::nullopt;
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(file_stat.st_size));
    std::array<char, 4096> buffer {};
    while (contents.size() <= max_environment_size) {
      const auto bytes = read(file_fd, buffer.data(), buffer.size());
      if (bytes == 0) {
        break;
      }
      if (bytes < 0) {
        if (errno == EINTR) {
          continue;
        }
        close(file_fd);
        return std::nullopt;
      }
      contents.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    close(file_fd);
    if (contents.size() > max_environment_size) {
      return std::nullopt;
    }
    return contents;
  }
}  // namespace

namespace platf::gamescope_session {
  bool valid_wayland_display(const std::string_view display_name) {
    // Wayland socket names are path components. Keeping discovery to a single
    // component prevents an environment file from redirecting capture outside
    // the already validated XDG runtime directory.
    if (display_name.empty() || display_name.size() > 107 || display_name == "." || display_name == "..") {
      return false;
    }
    for (const unsigned char ch : display_name) {
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) {
        return false;
      }
    }
    return true;
  }

  std::optional<environment_t> parse_environment(const std::string_view contents) {
    if (contents.size() > max_environment_size || contents.find('\0') != std::string_view::npos) {
      return std::nullopt;
    }

    environment_t result;
    for (std::size_t start = 0; start <= contents.size();) {
      const auto end = contents.find('\n', start);
      auto line = trim(contents.substr(start, end == std::string_view::npos ? contents.size() - start : end - start));
      if (line.starts_with("export ")) {
        line = trim(line.substr(7));
      }

      const auto equals = line.find('=');
      if (equals != std::string_view::npos) {
        const auto key = trim(line.substr(0, equals));
        if (key == "GAMESCOPE_WAYLAND_DISPLAY" || key == "DISPLAY") {
          auto value = parse_value(line.substr(equals + 1));
          auto &destination = key == "GAMESCOPE_WAYLAND_DISPLAY" ? result.wayland_display : result.x11_display;
          // Duplicate security-sensitive assignments are ambiguous; reject the
          // file instead of silently choosing first or last.
          if (!value || destination) {
            return std::nullopt;
          }
          destination = std::move(value);
        }
      }

      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }

    if (result.wayland_display && !valid_wayland_display(*result.wayland_display)) {
      return std::nullopt;
    }
    if (result.x11_display) {
      // Permit local Xwayland displays only, never a remote X11 server.
      auto display = std::string_view {*result.x11_display};
      if (display.size() < 2 || display.front() != ':') {
        return std::nullopt;
      }
      display.remove_prefix(1);
      bool seen_dot = false;
      for (std::size_t i = 0; i < display.size(); ++i) {
        const char ch = display[i];
        if (ch == '.' && !seen_dot && i > 0 && i + 1 < display.size()) {
          seen_dot = true;
        } else if (ch < '0' || ch > '9') {
          return std::nullopt;
        }
      }
    }
    return result;
  }

  bool import_x11_display() {
    const auto contents = read_environment_file(std::getenv("XDG_RUNTIME_DIR"));
    const auto environment = contents ? parse_environment(*contents) : std::nullopt;
    if (!environment || !environment->wayland_display || !environment->x11_display ||
        environment->wayland_display != discover_wayland_display()) {
      return false;
    }
    return setenv("DISPLAY", environment->x11_display->c_str(), 1) == 0;
  }

  std::optional<std::string> discover_wayland_display() {
    if (const char *display = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); display && valid_wayland_display(display)) {
      return std::string {display};
    }

    const auto contents = read_environment_file(std::getenv("XDG_RUNTIME_DIR"));
    if (!contents) {
      return std::nullopt;
    }
    const auto environment = parse_environment(*contents);
    if (!environment) {
      return std::nullopt;
    }
    return environment->wayland_display;
  }
}  // namespace platf::gamescope_session

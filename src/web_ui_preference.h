/**
 * @file web_ui_preference.h
 * @brief Isolated, host-wide Classic/V2 choice; never reloads the main config.
 */
#pragma once

#include <cctype>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "file_handler.h"

// Only the application-data directory accessor is needed by host().
// Avoid bringing platform/common.h (and its Boost/WinSock static
// initialization) into the isolated preference unit-test executable.
namespace platf {
  std::filesystem::path appdata();
}

namespace web_ui_preference {
  inline bool valid(const std::string_view choice) {
    return choice == "legacy" || choice == "v2";
  }

  inline std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.remove_suffix(1);
    }
    return text;
  }

  // Recognize only the exact webui key, not a commented-out or similarly named key.
  inline bool is_setting_line(std::string_view line) {
    const auto equal = line.find('=');
    return equal != std::string_view::npos && trim(line.substr(0, equal)) == "webui";
  }

  inline std::string parse_conf(const std::string_view content) {
    std::size_t offset = 0;
    while (offset < content.size()) {
      const auto end = content.find('\n', offset);
      const auto line = content.substr(offset, end == std::string_view::npos ? end : end - offset);
      if (is_setting_line(line)) {
        const auto value = line.substr(line.find('=') + 1);
        const auto parsed = trim(value.substr(0, value.find('#')));
        return valid(parsed) ? std::string(parsed) : "legacy";
      }
      if (end == std::string_view::npos) {
        break;
      }
      offset = end + 1;
    }
    return "legacy";
  }

  // Retain comments, other settings, whitespace and CRLF/LF line endings.
  // All repeated webui keys receive the same value so none can shadow another.
  inline std::string update_conf(const std::string_view original, const std::string_view choice) {
    if (original.empty()) {
      return "# Preferred Web UI for the system tray (legacy or v2).\nwebui = " + std::string(choice) + "\n";
    }
    std::string result;
    result.reserve(original.size() + choice.size() + 16);
    bool updated = false;
    std::size_t offset = 0;
    while (offset < original.size()) {
      const auto end = original.find('\n', offset);
      const auto line = original.substr(offset, end == std::string_view::npos ? end : end - offset);
      if (is_setting_line(line)) {
        const auto equal = line.find('=');
        auto value_start = equal + 1;
        while (value_start < line.size() && (line[value_start] == ' ' || line[value_start] == '\t')) {
          ++value_start;
        }
        const auto comment = line.find('#', value_start);
        auto value_end = comment == std::string_view::npos ? line.size() : comment;
        while (value_end > value_start && std::isspace(static_cast<unsigned char>(line[value_end - 1]))) {
          --value_end;
        }
        result.append(line.substr(0, value_start));
        result.append(choice);
        result.append(line.substr(value_end));
        updated = true;
      } else {
        result.append(line);
      }
      if (end == std::string_view::npos) {
        break;
      }
      result.push_back('\n');
      offset = end + 1;
    }
    if (!updated) {
      if (!result.empty() && result.back() != '\n') {
        result.push_back('\n');
      }
      result.append("webui = ");
      result.append(choice);
      result.push_back('\n');
    }
    return result;
  }

  // The real host uses atomic file replacement via file_handler. Injected I/O
  // keeps the storage and concurrent tray/open contract testable.
  class store_t {
  public:
    using reader_t = std::function<std::string(const std::filesystem::path &)>;
    using writer_t = std::function<bool(const std::filesystem::path &, std::string_view)>;

    store_t(std::filesystem::path path, reader_t reader, writer_t writer):
        path_(std::move(path)),
        reader_(std::move(reader)),
        writer_(std::move(writer)) {}

    std::string read() const {
      std::lock_guard lock(mutex_);
      return parse_conf(reader_(path_));
    }

    bool write(const std::string_view choice) {
      if (!valid(choice)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      return writer_(path_, update_conf(reader_(path_), choice));
    }

  private:
    std::filesystem::path path_;
    reader_t reader_;
    writer_t writer_;
    mutable std::mutex mutex_;
  };

  inline store_t &host() {
    static store_t store {
      platf::appdata() / "web-ui-preference.conf",
      [](const std::filesystem::path &path) {
        return file_handler::read_file(path.string().c_str());
      },
      [](const std::filesystem::path &path, const std::string_view choice) {
        return file_handler::write_file(path.string().c_str(), choice) == 0;
      },
    };
    return store;
  }
}  // namespace web_ui_preference

/**
 * @file web_ui_preference.h
 * @brief Isolated, host-wide Classic/V2 choice; never reloads the main config.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "file_handler.h"
#include "platform/common.h"

namespace web_ui_preference {
  inline bool valid(const std::string_view choice) {
    return choice == "legacy" || choice == "v2";
  }

  // The real host uses atomic file replacement via file_handler. Injected I/O
  // makes the storage and concurrent tray/open contract testable without
  // starting an HTTPS listener or changing a real user's configuration.
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
      const auto saved = reader_(path_);
      return valid(saved) ? saved : "legacy";
    }

    bool write(const std::string_view choice) {
      if (!valid(choice)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      return writer_(path_, choice);
    }

  private:
    std::filesystem::path path_;
    reader_t reader_;
    writer_t writer_;
    mutable std::mutex mutex_;
  };

  inline store_t &host() {
    static store_t store {
      platf::appdata() / "web-ui-preference",
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

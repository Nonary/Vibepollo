/**
 * @file src/file_handler.cpp
 * @brief Default operating-system adapter for file handling policy.
 */
#include "file_handler.h"

#include "logging.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #include <Windows.h>
#endif

namespace file_handler {
  namespace {
    class native_filesystem_t: public filesystem_t {
    public:
      bool exists(const std::filesystem::path &path) const override {
        std::error_code error;
        return std::filesystem::exists(path, error);
      }

      bool create_directories(const std::filesystem::path &path) override {
        std::error_code error;
        const bool created = std::filesystem::create_directories(path, error);
        return !error && (created || std::filesystem::exists(path));
      }

      std::string read(const std::filesystem::path &path) const override {
        std::ifstream input(path);
        return std::string {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
      }

      bool write(const std::filesystem::path &path, std::string_view contents) override {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
          return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        output.close();
        return static_cast<bool>(output);
      }

      replace_result_e replace(const std::filesystem::path &temporary,
                               const std::filesystem::path &target) override {
#ifdef _WIN32
        if (MoveFileExW(
              temporary.c_str(),
              target.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            )) {
          return replace_result_e::success;
        }
        return GetLastError() == ERROR_ACCESS_DENIED ? replace_result_e::access_denied : replace_result_e::error;
#else
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (!error) {
          return replace_result_e::success;
        }
        return error == std::errc::permission_denied ? replace_result_e::access_denied : replace_result_e::error;
#endif
      }

      std::optional<bool> read_only(const std::filesystem::path &path) const override {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          return std::nullopt;
        }
        return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
        (void) path;
        return false;
#endif
      }

      bool set_read_only(const std::filesystem::path &path, bool value) override {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          return false;
        }
        const auto updated = value ? attributes | FILE_ATTRIBUTE_READONLY : attributes & ~FILE_ATTRIBUTE_READONLY;
        return SetFileAttributesW(path.c_str(), updated) != 0;
#else
        (void) path;
        (void) value;
        return false;
#endif
      }

      void remove(const std::filesystem::path &path) override {
        std::error_code error;
        std::filesystem::remove(path, error);
      }

      std::filesystem::path temporary_path(const std::filesystem::path &target) override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
        auto temporary = target;
        temporary += ".tmp." + std::to_string(stamp) + "." + std::to_string(tid);
        return temporary;
      }
    };

    handler_t &default_handler() {
      static native_filesystem_t filesystem;
      static handler_t handler {filesystem, [](std::string_view message) {
        BOOST_LOG(error) << message;
      }};
      return handler;
    }
  }  // namespace

  std::string get_parent_directory(const std::string &path) {
    return default_handler().get_parent_directory(path);
  }

  bool make_directory(const std::string &path) {
    return default_handler().make_directory(path);
  }

  std::string read_file(const char *path) {
    return default_handler().read_file(path);
  }

  int write_file(const char *path, const std::string_view &contents) {
    return default_handler().write_file(path, contents);
  }
}  // namespace file_handler

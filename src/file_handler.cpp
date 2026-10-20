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
  #include "file_handler_windows.h"

  #include <Windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace file_handler {
  namespace {
#ifdef _WIN32
    struct windows_file_api_t {
      std::optional<HANDLE> create(const std::filesystem::path &path) {
        const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
          return std::nullopt;
        }
        return handle;
      }

      bool write(HANDLE handle, const char *data, uint32_t amount, uint32_t &written) {
        DWORD bytes_written = 0;
        const bool success = WriteFile(handle, data, amount, &bytes_written, nullptr) != 0;
        written = bytes_written;
        return success;
      }

      bool flush(HANDLE handle) {
        return FlushFileBuffers(handle) != 0;
      }

      bool close(HANDLE handle) {
        return CloseHandle(handle) != 0;
      }
    };
#endif

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
#ifdef _WIN32
        windows_file_api_t api;
        return detail::write_windows_file(api, path, contents);
#else
        // Atomic callers always write a fresh temporary path. O_EXCL and
        // O_NOFOLLOW prevent a same-directory attacker from redirecting a
        // predictable temporary name through a pre-positioned symlink.
        const int fd = ::open(
          path.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
          S_IRUSR | S_IWUSR
        );
        if (fd < 0) {
          return false;
        }

        size_t offset = 0;
        bool success = true;
        while (offset < contents.size()) {
          const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            success = false;
            break;
          }
          if (written == 0) {
            success = false;
            break;
          }
          offset += static_cast<size_t>(written);
        }

        if (success && ::fsync(fd) != 0) {
          success = false;
        }
        if (::close(fd) != 0) {
          success = false;
        }
        return success;
#endif
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

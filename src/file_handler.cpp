/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

// local includes
#include "file_handler.h"
#include "logging.h"

#ifdef _WIN32
  #include <Windows.h>
#endif

namespace file_handler {
  namespace {
    std::filesystem::path make_temp_path(const std::filesystem::path &target) {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
      std::filesystem::path tmp = target;
      tmp += ".tmp.";
      tmp += std::to_string(stamp);
      tmp += ".";
      tmp += std::to_string(tid);
      return tmp;
    }

    bool replace_file(const std::filesystem::path &tmp, const std::filesystem::path &target) {
#ifdef _WIN32
      if (!MoveFileExW(
            tmp.wstring().c_str(),
            target.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
          )) {
        BOOST_LOG(error) << "Failed to replace " << target.string() << " with " << tmp.string() << ": " << GetLastError();
        return false;
      }
      return true;
#else
      std::error_code ec;
      std::filesystem::rename(tmp, target, ec);
      if (ec) {
        BOOST_LOG(error) << "Failed to replace " << target.string() << " with " << tmp.string() << ": " << ec.message();
        return false;
      }
      return true;
#endif
    }
  }  // namespace

  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
    const std::filesystem::path target(path);
    const auto parent = target.parent_path();
    try {
      if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create parent directory for " << target.string() << ": " << e.what();
      return -1;
    }

    const auto tmp = make_temp_path(target);
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

    if (!out.is_open()) {
      return -1;
    }

    if (!contents.empty()) {
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    out.flush();
    if (!out.good()) {
      BOOST_LOG(error) << "Failed to write temporary file " << tmp.string();
      out.close();
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return -1;
    }
    out.close();
    if (!out) {
      BOOST_LOG(error) << "Failed to close temporary file " << tmp.string();
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return -1;
    }

    if (!replace_file(tmp, target)) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return -1;
    }

    return 0;
  }
}  // namespace file_handler

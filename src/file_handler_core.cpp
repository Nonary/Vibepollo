/**
 * @file src/file_handler_core.cpp
 * @brief Filesystem-independent file handling policy.
 */
#include "file_handler.h"

#include <utility>

namespace file_handler {
  handler_t::handler_t(filesystem_t &filesystem, diagnostic_t diagnostic):
      _filesystem(filesystem),
      _diagnostic(std::move(diagnostic)) {}

  void handler_t::report(std::string_view message) const {
    if (_diagnostic) {
      _diagnostic(message);
    }
  }

  std::string handler_t::get_parent_directory(const std::string &path) const {
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }
    return std::filesystem::path(trimmed_path).parent_path().string();
  }

  bool handler_t::make_directory(const std::string &path) {
    return _filesystem.exists(path) || _filesystem.create_directories(path);
  }

  std::string handler_t::read_file(const std::filesystem::path &path) const {
    if (!_filesystem.exists(path)) {
      return {};
    }
    return _filesystem.read(path);
  }

  int handler_t::write_file(const std::filesystem::path &path, std::string_view contents) {
    const auto parent = path.parent_path();
    if (!parent.empty() && !_filesystem.exists(parent) && !_filesystem.create_directories(parent)) {
      report("failed to create parent directory");
      return -1;
    }

    const auto temporary = _filesystem.temporary_path(path);
    if (!_filesystem.write(temporary, contents)) {
      _filesystem.remove(temporary);
      report("failed to write temporary file");
      return -1;
    }

    auto result = _filesystem.replace(temporary, path);
    if (result == replace_result_e::access_denied && _filesystem.read_only(path).value_or(false)) {
      if (_filesystem.set_read_only(path, false)) {
        result = _filesystem.replace(temporary, path);
        if (result != replace_result_e::success) {
          _filesystem.set_read_only(path, true);
        }
      }
    }

    if (result != replace_result_e::success) {
      _filesystem.remove(temporary);
      report("failed to replace target file");
      return -1;
    }
    return 0;
  }
}  // namespace file_handler

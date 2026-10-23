/**
 * @file src/file_handler_windows.h
 * @brief Windows temporary-file write sequence with an injectable OS boundary.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>

namespace file_handler::detail {
  template<class Api>
  bool write_windows_file(Api &api, const std::filesystem::path &path, std::string_view contents) {
    const auto handle = api.create(path);
    if (!handle) {
      return false;
    }

    size_t offset = 0;
    bool success = true;
    constexpr auto max_chunk_size = (std::numeric_limits<uint32_t>::max)();
    while (offset < contents.size()) {
      const auto amount = static_cast<uint32_t>(std::min<size_t>(contents.size() - offset, max_chunk_size));
      uint32_t written = 0;
      if (!api.write(*handle, contents.data() + offset, amount, written) || written == 0 || written > amount) {
        success = false;
        break;
      }
      offset += written;
    }

    // Flush the same writable handle before the caller can replace the target.
    // Closing a file or flushing a C++ stream alone does not flush OS buffers.
    if (success && !api.flush(*handle)) {
      success = false;
    }
    if (!api.close(*handle)) {
      success = false;
    }
    return success;
  }
}  // namespace file_handler::detail

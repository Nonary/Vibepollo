#include "src/platform/windows/display_helper_v2/file_text_storage.h"

#include <cstdio>
#include <memory>

namespace display_helper::v2 {
  std::optional<std::string> AtomicFileTextStorage::read(const std::string &key) {
    const std::filesystem::path path {key};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return std::nullopt;
    }
    FILE *file = _wfopen(path.wstring().c_str(), L"rb");
    if (!file) {
      return std::nullopt;
    }
    const auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
    std::string data;
    char buffer[4096];
    while (const size_t count = fread(buffer, 1, sizeof(buffer), file)) {
      data.append(buffer, count);
    }
    return data;
  }

  bool AtomicFileTextStorage::write_atomically(const std::string &key, const std::string &text) {
    const std::filesystem::path path {key};
    if (path.empty()) {
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += L".tmp";
    {
      FILE *file = _wfopen(temporary.wstring().c_str(), L"wb");
      if (!file) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)> {file, fclose};
      if (fwrite(text.data(), 1, text.size(), file) != text.size()) {
        guard.reset();
        std::filesystem::remove(temporary, ec);
        return false;
      }
    }
    if (!std::filesystem::exists(path, ec) || ec) {
      std::filesystem::rename(temporary, path, ec);
      if (!ec) {
        return true;
      }
    }
    std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      return false;
    }
    std::filesystem::remove(temporary, ec);
    return true;
  }

  bool AtomicFileTextStorage::remove(const std::string &key) {
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::path {key}, ec);
  }

  bool AtomicFileTextStorage::exists(const std::string &key) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path {key}, ec) && !ec;
  }
}  // namespace display_helper::v2

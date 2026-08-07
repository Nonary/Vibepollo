#include "src/platform/windows/display_helper_v2/text_storage.h"

namespace display_helper::v2 {
  std::optional<std::string> InMemoryTextStorage::read(const std::string &key) {
    const auto it = values_.find(key);
    return it == values_.end() ? std::nullopt : std::optional<std::string> {it->second};
  }

  bool InMemoryTextStorage::write_atomically(const std::string &key, const std::string &text) {
    if (key.empty()) {
      return false;
    }
    values_[key] = text;
    return true;
  }

  bool InMemoryTextStorage::remove(const std::string &key) {
    return values_.erase(key) != 0;
  }

  bool InMemoryTextStorage::exists(const std::string &key) {
    return values_.contains(key);
  }
}  // namespace display_helper::v2

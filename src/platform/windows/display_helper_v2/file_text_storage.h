#pragma once

#include "src/platform/windows/display_helper_v2/text_storage.h"

#include <filesystem>

namespace display_helper::v2 {
  /// Windows/runtime adapter: preserves the legacy durable atomic-write path.
  class AtomicFileTextStorage final : public ITextStorage {
  public:
    std::optional<std::string> read(const std::string &key) override;
    bool write_atomically(const std::string &key, const std::string &text) override;
    bool remove(const std::string &key) override;
    bool exists(const std::string &key) override;
  };
}  // namespace display_helper::v2

#pragma once

#include <map>
#include <optional>
#include <string>

namespace display_helper::v2 {
  /// Portable storage boundary for JSON/text persistence policy.  The core
  /// only knows stable string keys; production supplies atomic filesystem I/O.
  class ITextStorage {
  public:
    virtual ~ITextStorage() = default;

    virtual std::optional<std::string> read(const std::string &key) = 0;
    virtual bool write_atomically(const std::string &key, const std::string &text) = 0;
    virtual bool remove(const std::string &key) = 0;
    virtual bool exists(const std::string &key) = 0;
  };

  class InMemoryTextStorage final : public ITextStorage {
  public:
    std::optional<std::string> read(const std::string &key) override;
    bool write_atomically(const std::string &key, const std::string &text) override;
    bool remove(const std::string &key) override;
    bool exists(const std::string &key) override;

  private:
    std::map<std::string, std::string> values_;
  };
}  // namespace display_helper::v2

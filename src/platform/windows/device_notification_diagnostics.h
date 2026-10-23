#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace display_helper::device_diagnostics {
  // These tokens exist only in one event pump's lifetime. Raw interface/port
  // names stay in bounded memory; never emit them, serials or stable hashes.
  class context_t {
  public:
    static constexpr std::size_t max_name = 512;
    static constexpr std::size_t max_identities = 32;
    static constexpr unsigned max_messages = 8;
    using clock = std::chrono::steady_clock;

    std::optional<std::uint32_t> admit(clock::time_point now) {
      if (!window_ || now - *window_ >= std::chrono::seconds(10)) {
        window_ = now;
        emitted_ = 0;
      }
      if (emitted_ >= max_messages) {
        if (suppressed_ != std::numeric_limits<std::uint32_t>::max()) ++suppressed_;
        return std::nullopt;
      }
      ++emitted_;
      const auto suppressed = suppressed_;
      suppressed_ = 0;
      return suppressed;
    }

    unsigned identity(wchar_t type, std::wstring_view name) {
      if (name.empty() || name.size() > max_name) return 0;
      for (unsigned i = 0; i < identities_; ++i) {
        if (types_[i] == type && names_[i] == name) return i + 1;
      }
      if (identities_ == max_identities) return 0;
      types_[identities_] = type;
      names_[identities_] = name;
      return ++identities_;
    }

  private:
    std::optional<clock::time_point> window_;
    unsigned emitted_ = 0;
    std::uint32_t suppressed_ = 0;
    unsigned identities_ = 0;
    std::array<wchar_t, max_identities> types_ {};
    std::array<std::wstring, max_identities> names_ {};
  };

  // A public PnP model code helps distinguish displays without exposing the
  // instance/serial suffix. Accept exactly three letters plus four hex digits.
  inline std::string monitor_model(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"DISPLAY#";
    const auto upper = [](wchar_t c) { return c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c; };
    for (std::size_t pos = 0; pos + prefix.size() + 8 <= name.size(); ++pos) {
      if (pos && name[pos - 1] != L'\\') continue;
      bool matches = true;
      for (std::size_t i = 0; i < prefix.size(); ++i) matches &= upper(name[pos + i]) == prefix[i];
      if (!matches || name[pos + prefix.size() + 7] != L'#') continue;
      std::string model;
      for (std::size_t i = 0; i < 7; ++i) {
        const auto c = upper(name[pos + prefix.size() + i]);
        const bool valid = i < 3 ? c >= L'A' && c <= L'Z' :
          (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F');
        if (!valid) return "unknown";
        model.push_back(static_cast<char>(c));
      }
      return model;
    }
    return "unknown";
  }
}

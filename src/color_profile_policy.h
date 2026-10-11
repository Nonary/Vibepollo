#pragma once

#include <string_view>

namespace color_profile_policy {
  // Default permanent and temporary Sunshine monitor IDs remain identifiable
  // when optional display enumeration is unavailable or has incomplete metadata.
  // Custom EDIDs still use the caller's existing metadata classification fallback.
  inline bool is_sunshine_driver_monitor_path(std::wstring_view path) {
    constexpr std::wstring_view prefix = L"DISPLAY#SDD";
    const auto ascii_upper = [](wchar_t c) {
      return c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c;
    };
    for (std::size_t offset = 0; offset + prefix.size() + 5 <= path.size(); ++offset) {
      if (offset != 0 && path[offset - 1] != L'\\') {
        continue;
      }
      bool matches = true;
      for (std::size_t index = 0; index < prefix.size(); ++index) {
        matches = matches && ascii_upper(path[offset + index]) == prefix[index];
      }
      if (!matches) {
        continue;
      }
      const auto product = offset + prefix.size();
      if (path[product] != L'4' && path[product] != L'5') {
        continue;
      }
      for (std::size_t index = 1; index < 4; ++index) {
        const auto digit = ascii_upper(path[product + index]);
        matches = matches && ((digit >= L'0' && digit <= L'9') || (digit >= L'A' && digit <= L'F'));
      }
      if (matches && path[product + 4] == L'#') {
        return true;
      }
    }
    return false;
  }

  // A failed supported API must never be replaced by a registry-only association.
  template<class Native, class Legacy>
  bool associate(Native native, Legacy legacy) {
    const auto result = native();
    return result.api_available ? result.success : legacy();
  }
}

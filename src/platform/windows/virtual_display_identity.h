#pragma once

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>

// Shared by the host, helper and portable snapshot tests; no display queries.
namespace VDISPLAY {
  namespace identity_detail {
    inline bool equals_ascii_ci(const std::string_view lhs, const std::string_view rhs) {
      return lhs.size() == rhs.size() &&
             std::ranges::equal(lhs, rhs, [](const char left, const char right) {
               return std::tolower(static_cast<unsigned char>(left)) ==
                      std::tolower(static_cast<unsigned char>(right));
             });
    }

    inline bool contains_ascii_ci(const std::string_view haystack, const std::string_view needle) {
      if (needle.empty()) {
        return true;
      }
      return std::ranges::search(
               haystack,
               needle,
               [](const char left, const char right) {
                 return std::tolower(static_cast<unsigned char>(left)) ==
                        std::tolower(static_cast<unsigned char>(right));
               }
             ).begin() != haystack.end();
    }

    inline bool starts_with_ascii_ci(const std::string_view value, const std::string_view prefix) {
      return value.size() >= prefix.size() && equals_ascii_ci(value.substr(0, prefix.size()), prefix);
    }
  }  // namespace identity_detail

  inline bool is_sunshine_virtual_display_identity(
    const std::string &device_path,
    const std::string &friendly_name,
    const std::string &edid_manufacturer_id,
    const std::string &edid_product_code
  ) {
    if (identity_detail::contains_ascii_ci(device_path, "SunshineVirtualDisplay") ||
        identity_detail::contains_ascii_ci(device_path, "Sunshine Virtual Display")) {
      return true;
    }
    if (identity_detail::equals_ascii_ci(friendly_name, "Sunshine Virtual Display Driver")) {
      return true;
    }
    if (!identity_detail::equals_ascii_ci(edid_manufacturer_id, "SDD")) {
      return false;
    }
    return identity_detail::starts_with_ascii_ci(edid_product_code, "4") ||
           identity_detail::starts_with_ascii_ci(edid_product_code, "0x4") ||
           identity_detail::starts_with_ascii_ci(edid_product_code, "5") ||
           identity_detail::starts_with_ascii_ci(edid_product_code, "0x5");
  }
}  // namespace VDISPLAY

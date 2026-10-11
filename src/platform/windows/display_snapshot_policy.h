#pragma once

#include <display_device/types.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace display_helper_integration::snapshot_policy {
  namespace detail {
    inline std::string lowercase_ascii(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value;
    }

    inline std::string normalize_device_id(std::string value) {
      const auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
      });
      const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
      }).base();
      if (first >= last) {
        return {};
      }
      return lowercase_ascii(std::string(first, last));
    }

    inline bool contains_ci(const std::string &value, const std::string &needle) {
      return lowercase_ascii(value).find(lowercase_ascii(needle)) != std::string::npos;
    }

    inline bool equals_ci(const std::string &value, const std::string &expected) {
      return lowercase_ascii(value) == lowercase_ascii(expected);
    }

    inline bool is_active(const display_device::EnumeratedDevice &device) {
      return device.m_info.has_value() || !device.m_display_name.empty();
    }

    inline bool is_permanent_sunshine_vdd(const display_device::EnumeratedDevice &device) {
      if (!device.m_edid || !equals_ci(device.m_edid->m_manufacturer_id, "SDD")) {
        return false;
      }
      const auto &product_code = device.m_edid->m_product_code;
      return product_code.size() == 4 && product_code.front() == '4';
    }

    inline bool is_excluded(
      const display_device::EnumeratedDevice &device,
      const std::vector<std::string> &exclusions) {
      const auto id = device.m_device_id.empty() ? device.m_display_name : device.m_device_id;
      if (id.empty()) {
        return false;
      }
      const auto normalized_id = normalize_device_id(id);
      return std::any_of(exclusions.begin(), exclusions.end(), [&](const auto &exclusion) {
        return !normalized_id.empty() && normalize_device_id(exclusion) == normalized_id;
      });
    }
  }  // namespace detail

  inline bool is_virtual(const display_device::EnumeratedDevice &device) {
    if (detail::contains_ci(device.m_device_id, "SunshineVirtualDisplay") ||
        detail::contains_ci(device.m_device_id, "Sunshine Virtual Display") ||
        detail::contains_ci(device.m_display_name, "SunshineVirtualDisplay") ||
        detail::contains_ci(device.m_display_name, "Sunshine Virtual Display") ||
        detail::contains_ci(device.m_friendly_name, "SunshineVirtualDisplay") ||
        detail::contains_ci(device.m_friendly_name, "Sunshine Virtual Display")) {
      return true;
    }

    // SDD is Sunshine's bundled virtual display driver; SMK is legacy SudoVDA.
    return device.m_edid &&
           (detail::equals_ci(device.m_edid->m_manufacturer_id, "SDD") ||
            detail::equals_ci(device.m_edid->m_manufacturer_id, "SMK"));
  }

  inline bool may_capture_beside(
    const display_device::EnumeratedDevice &device,
    const std::vector<std::string> &exclusions) {
    if (!is_virtual(device) || !detail::is_active(device) ||
        detail::is_permanent_sunshine_vdd(device)) {
      return true;
    }
    return detail::is_excluded(device, exclusions);
  }

  inline bool blocks_capture(
    const display_device::EnumeratedDevice &device,
    const std::vector<std::string> &exclusions) {
    return !may_capture_beside(device, exclusions);
  }
}  // namespace display_helper_integration::snapshot_policy

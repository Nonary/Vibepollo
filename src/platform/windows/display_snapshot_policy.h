#pragma once

#include "virtual_display_identity.h"

#include <display_device/types.h>

#include <algorithm>
#include <array>
#include <cstdint>
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
    const auto manufacturer = device.m_edid ? device.m_edid->m_manufacturer_id : std::string {};
    const auto product = device.m_edid ? device.m_edid->m_product_code : std::string {};
    return VDISPLAY::is_sunshine_virtual_display_identity(device.m_device_id, device.m_friendly_name, manufacturer, product) ||
           // Older snapshots can carry the driver identity in either display name field.
           VDISPLAY::is_sunshine_virtual_display_identity(device.m_display_name, {}, {}, {}) ||
           VDISPLAY::is_sunshine_virtual_display_identity(device.m_friendly_name, {}, {}, {}) ||
           detail::equals_ci(manufacturer, "SMK");  // Legacy SudoVDA.
  }

  // Both backends retain encoder-probe displays under the shared GUID. Only that
  // exact tracked identity is exempt: other temporary/session targets still block.
  template<class TrackedTargets>
  void append_shared_capture_safe_devices(
    std::vector<std::string> &capture_safe_devices,
    const TrackedTargets &targets,
    const std::array<std::uint8_t, 16> &shared_guid) {
    for (const auto &target : targets) {
      if (target.guid_bytes != shared_guid || target.device_id.empty()) {
        continue;
      }
      const auto id = detail::normalize_device_id(target.device_id);
      if (std::none_of(capture_safe_devices.begin(), capture_safe_devices.end(), [&](const auto &existing) {
            return detail::normalize_device_id(existing) == id;
          })) {
        capture_safe_devices.push_back(target.device_id);
      }
    }
  }

  inline bool may_capture_beside(
    const display_device::EnumeratedDevice &device,
    const std::vector<std::string> &capture_safe_devices) {
    if (!is_virtual(device) || !detail::is_active(device) ||
        detail::is_permanent_sunshine_vdd(device)) {
      return true;
    }
    // Filtering a remembered session ID does not prove its physical layout is
    // safe. Only explicitly retained neighbors may bypass the active-VDD gate.
    return detail::is_excluded(device, capture_safe_devices);
  }

  inline bool blocks_capture(
    const display_device::EnumeratedDevice &device,
    const std::vector<std::string> &capture_safe_devices) {
    return !may_capture_beside(device, capture_safe_devices);
  }
}  // namespace display_helper_integration::snapshot_policy

/**
 * @file src/platform/windows/display_clone_group_policy.h
 * @brief Move a configured display out of a Duplicate group that blocks APPLY.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <display_device/types.h>

namespace display_helper::clone_group {
  /**
   * libdisplaydevice groups active displays that share a desktop origin. It
   * rejects any group larger than this, both as a requested layout and as the
   * layout it starts from.
   */
  inline constexpr std::size_t max_group_size = 2;

  /** Same shape as display_device::ActiveTopology, which is Windows-only. */
  using topology_t = std::vector<std::vector<std::string>>;

  struct active_display_t {
    std::string device_id;
    display_device::Point origin;
    display_device::Resolution resolution;
    std::optional<std::uint64_t> adapter;  ///< Source adapter LUID; empty when unknown.
  };

  inline bool same_device_id(const std::string &lhs, const std::string &rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](const char a, const char b) {
             return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
  }

  /**
   * @brief Find where to move a display so that it leaves an oversized Duplicate group.
   *
   * Windows can activate an arriving virtual display at the origin of a Duplicate
   * pair the user already has. libdisplaydevice then reports one group of three,
   * and every APPLY fails before it can reach the requested layout. Moving only the
   * configured display, to the right edge of the desktop, gives APPLY a valid
   * layout to start from.
   *
   * The display moves only when its adapter drives no other display in the group.
   * That is how a virtual display joins a Duplicate pair, and it means the user's
   * own displays are never rearranged.
   *
   * @param topology Current topology, grouped by shared origin.
   * @param device_id Display being configured.
   * @param active_displays Every active display with its desktop geometry and adapter.
   * @return The new origin, or std::nullopt when the display should stay where it is.
   */
  inline std::optional<display_device::Point> detach_origin(
    const topology_t &topology,
    const std::string &device_id,
    const std::vector<active_display_t> &active_displays
  ) {
    if (device_id.empty()) {
      return std::nullopt;
    }
    const auto find_display = [&](const std::string &id) -> const active_display_t * {
      const auto it = std::find_if(active_displays.begin(), active_displays.end(), [&](const auto &display) {
        return same_device_id(display.device_id, id);
      });
      return it == active_displays.end() ? nullptr : &*it;
    };

    const std::vector<std::string> *device_group = nullptr;
    for (const auto &group : topology) {
      if (group.empty()) {
        return std::nullopt;
      }
      const bool contains_device = std::any_of(group.begin(), group.end(), [&](const auto &id) {
        return same_device_id(id, device_id);
      });
      if (group.size() - (contains_device ? 1 : 0) > max_group_size) {
        // Some group stays too large after the move, so APPLY would still fail.
        return std::nullopt;
      }
      if (contains_device) {
        device_group = &group;
      }
    }
    if (!device_group || device_group->size() <= max_group_size) {
      return std::nullopt;
    }

    const auto *device = find_display(device_id);
    if (!device || !device->adapter) {
      return std::nullopt;
    }
    for (const auto &id : *device_group) {
      if (same_device_id(id, device_id)) {
        continue;
      }
      const auto *member = find_display(id);
      if (!member || !member->adapter || *member->adapter == *device->adapter) {
        return std::nullopt;
      }
    }

    // Top-align with the rightmost display so the two share an edge.
    const active_display_t *rightmost = nullptr;
    std::int64_t right_edge = 0;
    for (const auto &display : active_displays) {
      if (&display == device) {
        continue;
      }
      const auto edge = static_cast<std::int64_t>(display.origin.m_x) + display.resolution.m_width;
      if (!rightmost || edge > right_edge) {
        rightmost = &display;
        right_edge = edge;
      }
    }
    // Parenthesized: <windows.h> may define a max() macro.
    if (!rightmost || right_edge > (std::numeric_limits<int>::max)()) {
      return std::nullopt;
    }
    return display_device::Point {static_cast<int>(right_edge), rightmost->origin.m_y};
  }
}  // namespace display_helper::clone_group

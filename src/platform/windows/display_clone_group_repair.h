/**
 * @file src/platform/windows/display_clone_group_repair.h
 * @brief Move a configured display out of a Duplicate group that blocks APPLY.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <display_device/windows/win_api_layer_interface.h>
#include <display_device/windows/win_api_utils.h>
#include <display_device/windows/win_display_device_interface.h>

#include "src/logging.h"
#include "src/platform/windows/display_clone_group_policy.h"

namespace display_helper::clone_group {
  /**
   * @brief Move the configured display out of a Duplicate group that blocks APPLY.
   *
   * Windows can activate an arriving virtual display at the origin of a Duplicate
   * pair the user already has. libdisplaydevice then sees one group of three and
   * refuses to change the layout at all, so every APPLY fails (Vibepollo#702).
   * See detach_origin() for when the display is moved. The caller holds the
   * native mutation scope.
   */
  inline void detach_before_apply(
    display_device::WinDisplayDeviceInterface &display,
    const display_device::WinApiLayerInterface &win_api,
    const std::string &device_id
  ) {
    if (device_id.empty()) {
      return;
    }
    try {
      const auto topology = display.getCurrentTopology();
      if (topology.empty() || display.isTopologyValid(topology)) {
        return;
      }

      const auto active = win_api.queryDisplayConfig(display_device::QueryType::Active);
      std::vector<active_display_t> displays;
      for (const auto &device : display.enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
        if (!device.m_info) {
          continue;
        }
        std::optional<std::uint64_t> adapter;
        if (active) {
          if (const auto *path = display_device::win_utils::getActivePath(win_api, device.m_device_id, active->m_paths)) {
            adapter = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(path->sourceInfo.adapterId.HighPart)) << 32) |
                      path->sourceInfo.adapterId.LowPart;
          }
        }
        displays.push_back({device.m_device_id, device.m_info->m_origin_point, device.m_info->m_resolution, adapter});
      }

      const auto origin = detach_origin(topology, device_id, displays);
      if (!origin) {
        BOOST_LOG(warning) << "Display helper: current topology is invalid; display " << device_id
                           << " cannot be moved out of it.";
        return;
      }
      BOOST_LOG(warning) << "Display helper: display " << device_id
                         << " shares its position with more than one other display; moving it to ("
                         << origin->m_x << ", " << origin->m_y << ") before applying.";
      if (!display.setDisplayOrigin(device_id, *origin)) {
        BOOST_LOG(warning) << "Display helper: failed to move display " << device_id << " out of its Duplicate group.";
      }
    } catch (...) {
      BOOST_LOG(warning) << "Display helper: failed to check display " << device_id << " for an oversized Duplicate group.";
    }
  }
}  // namespace display_helper::clone_group

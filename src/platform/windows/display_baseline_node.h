#pragma once

#include "display_snapshot_policy.h"
#include "src/remote_display_topology.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace display_helper_integration {
  inline std::optional<remote_display_topology::node_t> baseline_node(const display_device::EnumeratedDevice &device) {
    if (device.m_device_id.empty() || device.m_display_name.empty() || !device.m_info) return std::nullopt;
    const auto &info = *device.m_info;
    remote_display_topology::node_t node;
    node.id = node.device_id = device.m_device_id;
    node.label = device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name;
    node.preexisting = node.active = true;
    node.physical = !snapshot_policy::is_virtual(device);
    node.primary = info.m_primary;
    node.x = info.m_origin_point.m_x;
    node.y = info.m_origin_point.m_y;
    node.configured_mode.width = static_cast<int>(info.m_resolution.m_width);
    node.configured_mode.height = static_cast<int>(info.m_resolution.m_height);
    node.configured_mode.hdr = info.m_hdr_state.value_or(display_device::HdrState::Disabled) == display_device::HdrState::Enabled;
    if (const auto *rate = std::get_if<display_device::Rational>(&info.m_refresh_rate)) {
      node.baseline_refresh_rate = {rate->m_numerator, rate->m_denominator};
      node.configured_mode.refresh_hz = rate->m_denominator ? static_cast<int>(std::lround(static_cast<double>(rate->m_numerator) / rate->m_denominator)) : 0;
    } else if (const auto *rate = std::get_if<double>(&info.m_refresh_rate)) {
      node.configured_mode.refresh_hz = static_cast<int>(std::lround(*rate));
    }
    return node;
  }

  inline std::optional<std::vector<remote_display_topology::node_t>> physical_baseline_nodes(
    const display_device::EnumeratedDeviceList &devices, const std::vector<std::string> &capture_safe_devices) {
    if (std::any_of(devices.begin(), devices.end(), [&](const auto &device) { return snapshot_policy::blocks_capture(device, capture_safe_devices); })) return std::nullopt;
    std::vector<remote_display_topology::node_t> nodes;
    for (const auto &device : devices) {
      if (auto node = baseline_node(device); node && node->physical) nodes.push_back(std::move(*node));
    }
    return nodes;
  }
}

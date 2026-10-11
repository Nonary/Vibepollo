#pragma once

#include <display_device/windows/win_api_layer_interface.h>
#include <boost/algorithm/string/predicate.hpp>
#include <algorithm>

namespace display_helper_integration {
  inline std::optional<std::string> disconnected_baseline_device(
    const display_device::WinApiLayerInterface &api, const std::vector<std::string> &baseline_ids) {
    if (baseline_ids.empty()) return std::nullopt;
    // Failed restore can leave connected targets inactive and without source
    // modes/names. Inspect target availability directly, not enumerated sources.
    const auto data = api.queryDisplayConfig(display_device::QueryType::All);
    if (!data) return std::nullopt;
    std::vector<std::string> connected;
    for (const auto &path : data->m_paths) {
      if (!path.targetInfo.targetAvailable) continue;
      const auto id = api.getDeviceId(path);
      if (id.empty()) return std::nullopt;  // Could be the saved monitor.
      connected.push_back(id);
    }
    if (connected.empty()) return std::nullopt;
    for (const auto &id : baseline_ids) {
      if (std::none_of(connected.begin(), connected.end(), [&](const auto &present) {
            return boost::iequals(id, present);
          })) return id;
    }
    return std::nullopt;
  }
}

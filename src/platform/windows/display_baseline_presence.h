#pragma once

#include <display_device/windows/win_api_layer_interface.h>
#include <display_device/windows/win_api_recovery.h>
#include <boost/algorithm/string/predicate.hpp>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>
#include <boost/algorithm/string/case_conv.hpp>

namespace display_helper_integration {
  inline std::optional<std::vector<std::string>> disconnected_baseline_devices(
    const display_device::WinApiLayerInterface &api, const std::vector<std::string> &baseline_ids) {
    if (baseline_ids.empty()) return std::vector<std::string> {};
    // Failed restore can leave connected targets inactive and without source
    // modes/names. Inspect target availability directly, not enumerated sources.
    display_device::DisplayRecoveryBehaviorGuard recovery(display_device::DisplayRecoveryBehavior::Skip);
    const auto data = api.queryDisplayConfig(display_device::QueryType::All);
    if (!data) return std::nullopt;
    std::unordered_set<std::string> connected;
    for (const auto &path : data->m_paths) {
      if (!path.targetInfo.targetAvailable) continue;
      const auto id = api.getDeviceId(path);
      if (id.empty()) return std::nullopt;  // Could be the saved monitor.
      connected.insert(boost::to_lower_copy(id));
    }
    std::vector<std::string> missing;
    for (const auto &id : baseline_ids) {
      if (!connected.contains(boost::to_lower_copy(id))) missing.push_back(id);
    }
    return missing;
  }
}

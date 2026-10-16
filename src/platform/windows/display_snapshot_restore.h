#pragma once

#include <set>
#include <string>

namespace display_helper {
  // Coordinator geometry must be restored before its best-effort refresh stage.
  // Use the same temporary exact-mode operation as snapshot restoration below.
  // Return geometry success so a refresh-only failure cannot latch the baseline guard.
  template <typename Device, typename Topology, typename ReportFailure>
  bool restore_composed_topology(Device &device, const Topology &layout, ReportFailure report_failure) {
    if (!layout.topology.empty() && !device.isTopologyTheSame(device.getCurrentTopology(), layout.topology) &&
        !device.setTopology(layout.topology)) {
      report_failure("topology", "");
      return false;
    }
    if (layout.primary_device && !layout.primary_device->empty() && !device.setAsPrimary(*layout.primary_device)) {
      report_failure("primary", *layout.primary_device);
      return false;
    }
    for (const auto &[id, origin] : layout.monitor_positions) {
      if (!device.setDisplayOrigin(id, origin)) {
        report_failure("origin", id);
        return false;
      }
    }
    if (layout.device_refresh_rate_overrides.empty()) return true;
    std::set<std::string> requested;
    for (const auto &[id, rate] : layout.device_refresh_rate_overrides) {
      requested.insert(id);
    }
    const auto report_refresh_failure = [&](const char *reason) {
      decltype(device.getCurrentDisplayModes(requested)) readback;
      try {
        readback = device.getCurrentDisplayModes(requested);
      } catch (...) {
        // Failure to read back is diagnostic, not a geometry failure.
      }
      for (const auto &id : requested) {
        const auto &rate = layout.device_refresh_rate_overrides.at(id);
        const auto found = readback.find(id);
        const auto actual = found == readback.end() ? std::string {"unavailable"} :
          std::to_string(found->second.m_refresh_rate.m_numerator) + "/" +
          std::to_string(found->second.m_refresh_rate.m_denominator);
        report_failure(std::string {reason} + " (requested " + std::to_string(rate.first) + "/" +
                         std::to_string(rate.second) + ", readback " + actual + ")", id);
      }
    };
    try {
      std::set<std::string> active;
      for (const auto &group : device.getCurrentTopology()) active.insert(group.begin(), group.end());
      std::erase_if(requested, [&](const auto &id) { return !active.contains(id); });
      // Exclusive APPLY deliberately keeps physical refresh targets inactive.
      if (requested.empty()) return true;
      auto modes = device.getCurrentDisplayModes(requested);
      for (const auto &id : requested) {
        const auto found = modes.find(id);
        if (found == modes.end()) {
          report_refresh_failure("refresh read");
          return true;
        }
        const auto &rate = layout.device_refresh_rate_overrides.at(id);
        found->second.m_refresh_rate = {rate.first, rate.second};
      }
      if (!device.setDisplayModesExactTemporary(modes)) {
        report_refresh_failure("exact refresh");
      }
    } catch (...) {
      report_refresh_failure("refresh exception");
    }
    return true;
  }

  // Topology must be ready before entering this stage. In particular, do not
  // save primary/origin changes after a rejected or substituted restore mode.
  template <typename Device, typename Snapshot>
  bool restore_snapshot_settings(Device &device, const Snapshot &snapshot) {
    if (!snapshot.m_modes.empty() && !device.setDisplayModesExactTemporary(snapshot.m_modes)) {
      return false;
    }
    if (!snapshot.m_hdr_states.empty() && !device.setHdrStates(snapshot.m_hdr_states)) {
      return false;
    }
    if (!snapshot.m_primary_device.empty() && !device.setAsPrimary(snapshot.m_primary_device)) {
      return false;
    }
    for (const auto &[id, origin] : snapshot.m_origins) {
      if (!device.setDisplayOrigin(id, origin)) {
        return false;
      }
    }
    return true;
  }
}  // namespace display_helper

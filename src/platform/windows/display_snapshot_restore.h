#pragma once

#include <set>
#include <string>

namespace display_helper {
  // Layout mode-sets do not promise to preserve color state. Apply only known
  // saved states after the last layout change, then check the active devices.
  template <typename Device, typename Snapshot>
  bool restore_snapshot_hdr_states(Device &device, const Snapshot &snapshot) {
    auto states = snapshot.m_hdr_states;
    std::set<std::string> ids;
    for (auto it = states.begin(); it != states.end();) {
      if (!it->second) {
        it = states.erase(it);
      } else {
        ids.insert(it->first);
        ++it;
      }
    }
    if (states.empty()) {
      return true;
    }
    if (!device.setHdrStates(states)) {
      return false;
    }
    const auto current = device.getCurrentHdrStates(ids);
    for (const auto &[id, state] : states) {
      const auto it = current.find(id);
      if (it == current.end() || it->second != state) {
        return false;
      }
    }
    return true;
  }

  // Topology must be ready before entering this stage. In particular, do not
  // save primary/origin changes after a rejected or substituted restore mode.
  template <typename Device, typename Snapshot>
  bool restore_snapshot_settings(Device &device, const Snapshot &snapshot, bool restore_hdr = true) {
    if (!snapshot.m_modes.empty() && !device.setDisplayModesExactTemporary(snapshot.m_modes)) {
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
    // Callers with a later rotation stage defer HDR until that stage completes.
    return !restore_hdr || restore_snapshot_hdr_states(device, snapshot);
  }
}  // namespace display_helper

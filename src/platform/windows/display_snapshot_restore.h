#pragma once

namespace display_helper {
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

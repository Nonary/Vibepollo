#pragma once

#include "src/platform/windows/display_helper_v2/types.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace display_helper::v2::topology {
  using InitialState = display_device::SingleDisplayConfigState::Initial;
  using TopologyMetadata = std::tuple<ActiveTopology, std::string, std::set<std::string>>;

  // Value comparisons are kept here so V2's portable core never requires the
  // libdisplaydevice Windows implementation merely to compare value objects.
  bool equal_display_modes(const display_device::DeviceDisplayModeMap &lhs,
                           const display_device::DeviceDisplayModeMap &rhs);
  bool equal_origins(const std::map<std::string, display_device::Point> &lhs,
                     const std::map<std::string, display_device::Point> &rhs);
  bool equal_snapshot(const Snapshot &lhs, const Snapshot &rhs);
  bool equal_initial(const InitialState &lhs, const InitialState &rhs);

  std::optional<InitialState> compute_initial_state(
    const std::optional<InitialState> &previous_state,
    const ActiveTopology &topology_before_changes,
    const EnumeratedDeviceList &devices);
  std::optional<InitialState> strip_initial_state(
    const InitialState &initial_state,
    const EnumeratedDeviceList &devices);
  TopologyMetadata compute_new_topology_and_metadata(
    SingleDisplayConfiguration::DevicePreparation device_preparation,
    const std::string &device_id,
    const InitialState &initial_state);
}  // namespace display_helper::v2::topology

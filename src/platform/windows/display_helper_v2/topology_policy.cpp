#include "src/platform/windows/display_helper_v2/topology_policy.h"

#include <algorithm>
#include <iterator>

namespace display_helper::v2::topology {
  namespace {
    std::set<std::string> device_ids(const EnumeratedDeviceList &devices, const bool primary_only) {
      std::set<std::string> ids;
      for (const auto &device : devices) {
        if (!primary_only || (device.m_info && device.m_info->m_primary)) {
          ids.insert(device.m_device_id);
        }
      }
      return ids;
    }

    ActiveTopology strip_topology(const ActiveTopology &topology, const EnumeratedDeviceList &devices) {
      const auto available = device_ids(devices, false);
      ActiveTopology stripped;
      for (const auto &group : topology) {
        std::vector<std::string> filtered;
        for (const auto &id : group) {
          if (available.contains(id)) {
            filtered.push_back(id);
          }
        }
        if (!filtered.empty()) {
          stripped.push_back(std::move(filtered));
        }
      }
      return stripped;
    }

    std::set<std::string> strip_devices(const std::set<std::string> &ids, const EnumeratedDeviceList &devices) {
      const auto available = device_ids(devices, false);
      std::set<std::string> stripped;
      std::ranges::set_intersection(ids, available, std::inserter(stripped, stripped.begin()));
      return stripped;
    }

    std::set<std::string> flatten_topology(const ActiveTopology &topology) {
      std::set<std::string> flattened;
      for (const auto &group : topology) {
        flattened.insert(group.begin(), group.end());
      }
      return flattened;
    }

    std::set<std::string> other_devices_in_group(const ActiveTopology &topology, const std::string &target) {
      for (const auto &group : topology) {
        if (std::ranges::find(group, target) != group.end()) {
          std::set<std::string> others(group.begin(), group.end());
          others.erase(target);
          return others;
        }
      }
      return {};
    }

    ActiveTopology compute_new_topology(
      const SingleDisplayConfiguration::DevicePreparation device_preparation,
      const bool configuring_primary_devices,
      const std::string &device_to_configure,
      const std::set<std::string> &additional_devices,
      const ActiveTopology &initial_topology) {
      using DevicePreparation = SingleDisplayConfiguration::DevicePreparation;
      if (device_preparation == DevicePreparation::VerifyOnly) {
        return initial_topology;
      }
      if (device_preparation == DevicePreparation::EnsureOnlyDisplay) {
        if (!configuring_primary_devices) {
          return {{device_to_configure}};
        }
        std::vector<std::string> primary_group {device_to_configure};
        primary_group.insert(primary_group.end(), additional_devices.begin(), additional_devices.end());
        return {std::move(primary_group)};
      }
      if (!flatten_topology(initial_topology).contains(device_to_configure)) {
        auto topology = initial_topology;
        topology.push_back({device_to_configure});
        return topology;
      }
      return initial_topology;
    }
  }  // namespace

  bool equal_display_modes(const display_device::DeviceDisplayModeMap &lhs,
                           const display_device::DeviceDisplayModeMap &rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (const auto &[id, lhs_mode] : lhs) {
      const auto rhs_it = rhs.find(id);
      if (rhs_it == rhs.end() ||
          lhs_mode.m_resolution.m_width != rhs_it->second.m_resolution.m_width ||
          lhs_mode.m_resolution.m_height != rhs_it->second.m_resolution.m_height ||
          lhs_mode.m_refresh_rate.m_numerator != rhs_it->second.m_refresh_rate.m_numerator ||
          lhs_mode.m_refresh_rate.m_denominator != rhs_it->second.m_refresh_rate.m_denominator) {
        return false;
      }
    }
    return true;
  }

  bool equal_origins(const std::map<std::string, display_device::Point> &lhs,
                     const std::map<std::string, display_device::Point> &rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (const auto &[id, lhs_origin] : lhs) {
      const auto rhs_it = rhs.find(id);
      if (rhs_it == rhs.end() || lhs_origin.m_x != rhs_it->second.m_x || lhs_origin.m_y != rhs_it->second.m_y) {
        return false;
      }
    }
    return true;
  }

  bool equal_snapshot(const Snapshot &lhs, const Snapshot &rhs) {
    if (lhs.m_topology != rhs.m_topology ||
        !equal_display_modes(lhs.m_modes, rhs.m_modes) ||
        lhs.m_hdr_states != rhs.m_hdr_states ||
        lhs.m_primary_device != rhs.m_primary_device) {
      return false;
    }
    return lhs.m_origins.empty() || rhs.m_origins.empty() || equal_origins(lhs.m_origins, rhs.m_origins);
  }

  bool equal_initial(const InitialState &lhs, const InitialState &rhs) {
    return lhs.m_topology == rhs.m_topology && lhs.m_primary_devices == rhs.m_primary_devices;
  }

  std::optional<InitialState> compute_initial_state(
    const std::optional<InitialState> &previous_state,
    const ActiveTopology &topology_before_changes,
    const EnumeratedDeviceList &devices) {
    if (previous_state) {
      return *previous_state;
    }
    const auto primary_devices = device_ids(devices, true);
    if (primary_devices.empty()) {
      return std::nullopt;
    }
    return InitialState {topology_before_changes, primary_devices};
  }

  std::optional<InitialState> strip_initial_state(const InitialState &initial_state,
                                                   const EnumeratedDeviceList &devices) {
    const auto stripped_topology = strip_topology(initial_state.m_topology, devices);
    auto primary_devices = strip_devices(initial_state.m_primary_devices, devices);

    if (stripped_topology.empty()) {
      auto current_primary_devices = device_ids(devices, true);
      if (current_primary_devices.empty()) {
        current_primary_devices = device_ids(devices, false);
      }
      if (current_primary_devices.empty()) {
        return std::nullopt;
      }
      return InitialState {ActiveTopology {}, std::move(current_primary_devices)};
    }

    if (primary_devices.empty()) {
      primary_devices = device_ids(devices, true);
      if (primary_devices.empty()) {
        return std::nullopt;
      }
    }
    return InitialState {stripped_topology, std::move(primary_devices)};
  }

  TopologyMetadata compute_new_topology_and_metadata(
    const SingleDisplayConfiguration::DevicePreparation device_preparation,
    const std::string &device_id,
    const InitialState &initial_state) {
    const bool configuring_primary_devices = device_id.empty();
    std::string device_to_configure = device_id;
    std::set<std::string> additional_devices;

    if (configuring_primary_devices) {
      for (const auto &group : initial_state.m_topology) {
        const auto primary = std::ranges::find_if(group, [&initial_state](const auto &candidate) {
          return initial_state.m_primary_devices.contains(candidate);
        });
        if (primary != group.end()) {
          device_to_configure = *primary;
          break;
        }
      }
      if (device_to_configure.empty() && !initial_state.m_primary_devices.empty()) {
        device_to_configure = *initial_state.m_primary_devices.begin();
      }
      if (device_to_configure.empty()) {
        for (const auto &group : initial_state.m_topology) {
          if (!group.empty()) {
            device_to_configure = group.front();
            break;
          }
        }
      }
    }

    if (!device_to_configure.empty()) {
      additional_devices = other_devices_in_group(initial_state.m_topology, device_to_configure);
    }
    auto new_topology = compute_new_topology(
      device_preparation,
      configuring_primary_devices,
      device_to_configure,
      additional_devices,
      initial_state.m_topology);
    const auto flattened = flatten_topology(new_topology);
    if (!device_to_configure.empty() && !flattened.contains(device_to_configure)) {
      device_to_configure = flattened.empty() ? "" : *flattened.begin();
    }
    additional_devices = device_to_configure.empty() ? std::set<std::string> {} :
                                                        other_devices_in_group(new_topology, device_to_configure);
    return {std::move(new_topology), std::move(device_to_configure), std::move(additional_devices)};
  }
}  // namespace display_helper::v2::topology

#pragma once

#include "src/platform/windows/display_helper_v2/types.h"

#include <optional>
#include <display_device/windows/settings_utils.h>

namespace display_helper::v2 {
  /**
   * @brief State transformations for applying settings after topology activation.
   *
   * The staged engine owns topology transitions, while SettingsManager retains
   * the original primary/mode/HDR values needed by consecutive APPLY requests.
   */
  class StagedSettingsState {
  public:
    static std::optional<display_device::SingleDisplayConfigState::Initial> topology_base(
      const std::optional<display_device::SingleDisplayConfigState::Initial> &session_initial,
      const ActiveTopology &current_topology,
      const EnumeratedDeviceList &devices) {
      if (session_initial) {
        return display_device::win_utils::stripInitialState(*session_initial, devices);
      }
      return display_device::win_utils::computeInitialState(
        std::nullopt,
        current_topology,
        devices);
    }

    /// Build the SettingsManager base after the staged topology has settled.
    /// Its topology must describe what Windows actually accepted, while an
    /// omitted device id must still resolve through the original primary group
    /// captured at the start of the helper session.
    static std::optional<display_device::SingleDisplayConfigState::Initial> rebased_initial(
      const std::optional<display_device::SingleDisplayConfigState::Initial> &session_initial,
      const ActiveTopology &current_topology,
      const EnumeratedDeviceList &devices) {
      auto current = topology_base(std::nullopt, current_topology, devices);
      if (!current || !session_initial) {
        return current;
      }

      const auto retained = display_device::win_utils::stripInitialState(*session_initial, devices);
      if (retained) {
        current->m_primary_devices = retained->m_primary_devices;
      }
      return current;
    }

    static display_device::SingleDisplayConfigState rebase(
      const std::optional<display_device::SingleDisplayConfigState> &previous,
      const display_device::SingleDisplayConfigState::Initial &current_initial,
      const ActiveTopology &current_topology) {
      display_device::SingleDisplayConfigState rebased;
      rebased.m_initial = current_initial;
      if (previous) {
        rebased.m_modified = previous->m_modified;
      }
      rebased.m_modified.m_topology = current_topology;
      return rebased;
    }

    static SingleDisplayConfiguration settings_configuration(
      const SingleDisplayConfiguration &requested) {
      auto settings = requested;
      if (settings.m_device_prep !=
          SingleDisplayConfiguration::DevicePreparation::EnsurePrimary) {
        settings.m_device_prep = SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
      }
      return settings;
    }
  };
}  // namespace display_helper::v2

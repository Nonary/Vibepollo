#include "src/platform/windows/display_helper_request_policy.h"

#include <algorithm>

namespace display_helper_integration::request_policy {
  namespace {
    bool is_extended(const VirtualDisplayLayout layout) {
      return layout != VirtualDisplayLayout::Exclusive;
    }

    bool contains_device(const std::vector<std::vector<std::string>> &topology, const std::string &device_id) {
      return std::any_of(topology.begin(), topology.end(), [&](const auto &group) {
        return std::find(group.begin(), group.end(), device_id) != group.end();
      });
    }
  }  // namespace

  bool virtual_display_mutation_allowed(const bool display_restore_in_progress) {
    return !display_restore_in_progress;
  }

  bool supersede_restore_for_virtual_display(
    const std::function<void()> &disarm_restore,
    const std::function<bool()> &restore_in_progress
  ) {
    disarm_restore();
    return virtual_display_mutation_allowed(restore_in_progress());
  }

  Result evaluate(const Input &input) {
    Result result;
    // A failed virtual-display attempt falls back to a physical target. An
    // explicitly selected HDR profile remains valid for that physical target.
    result.apply_hdr_profile_to_physical = input.hdr_profile_selected && !input.virtual_display;

    if (input.virtual_display && input.target_device_id.empty()) {
      result.dispatch = false;
      return result;
    }

    if (!input.virtual_display &&
        input.physical_output_override &&
        input.configuration_option == ConfigurationOption::Disabled) {
      result.dispatch = false;
      return result;
    }

    if (input.virtual_display &&
        input.configuration_option == ConfigurationOption::Disabled &&
        is_extended(input.layout)) {
      result.dispatch = false;
    }

    if (input.virtual_display) {
      switch (input.layout) {
        case VirtualDisplayLayout::Exclusive:
          result.device_preparation = DevicePreparation::EnsureOnlyDisplay;
          break;
        case VirtualDisplayLayout::Extended:
        case VirtualDisplayLayout::ExtendedIsolated:
          result.device_preparation = DevicePreparation::EnsureActive;
          break;
        case VirtualDisplayLayout::ExtendedPrimary:
        case VirtualDisplayLayout::ExtendedPrimaryIsolated:
          result.device_preparation = DevicePreparation::EnsurePrimary;
          break;
      }
    }

    if (input.rtx_hdr_source_enabled && input.hdr_requested) {
      result.hdr_enabled = false;
    }

    if (input.remapped_resolution) {
      result.initial_resolution = input.remapped_resolution;
      result.applied_resolution = input.remapped_resolution;
    }

    if (input.virtual_display && is_extended(input.layout)) {
      result.topology = input.topology_snapshot;
      if (!result.topology.empty() && !input.target_device_id.empty() && !contains_device(result.topology, input.target_device_id)) {
        result.topology.push_back({input.target_device_id});
      }
    } else if ((input.virtual_display && input.layout == VirtualDisplayLayout::Exclusive) ||
               (!input.virtual_display && input.configuration_option == ConfigurationOption::EnsureOnlyDisplay)) {
      if (!input.target_device_id.empty()) {
        result.topology = {{input.target_device_id}};
      }
    }

    return result;
  }
}  // namespace display_helper_integration::request_policy

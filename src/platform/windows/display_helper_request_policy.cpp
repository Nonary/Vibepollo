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

  Result evaluate(const Input &input) {
    Result result;
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

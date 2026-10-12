#include "src/platform/windows/display_helper_request_policy.h"

#include <algorithm>
#include <cctype>

namespace display_helper_integration::request_policy {
  namespace {
    bool is_extended(const VirtualDisplayLayout layout) {
      return layout != VirtualDisplayLayout::Exclusive;
    }

    bool same_device(const std::string &left, const std::string &right) {
      return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](unsigned char l, unsigned char r) {
        return std::tolower(l) == std::tolower(r);
      });
    }

    bool contains_device(const std::vector<std::vector<std::string>> &topology, const std::string &device_id) {
      return std::any_of(topology.begin(), topology.end(), [&](const auto &group) {
        return std::any_of(group.begin(), group.end(), [&](const auto &id) { return same_device(id, device_id); });
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

  std::vector<std::vector<std::string>> merge_extended_topology(
    std::vector<std::vector<std::string>> current,
    const std::vector<std::vector<std::string>> &baseline,
    const std::string &target_device_id
  ) {
    for (const auto &group : baseline) {
      auto existing = std::find_if(current.begin(), current.end(), [&](const auto &candidate) {
        return std::any_of(group.begin(), group.end(), [&](const auto &id) {
          return std::any_of(candidate.begin(), candidate.end(), [&](const auto &live) { return same_device(id, live); });
        });
      });
      std::vector<std::string> missing;
      for (const auto &id : group) {
        if (!contains_device(current, id)) missing.push_back(id);
      }
      if (missing.empty()) continue;
      if (existing == current.end()) current.push_back(std::move(missing));
      else existing->insert(existing->end(), missing.begin(), missing.end());
    }
    if (!target_device_id.empty() && !contains_device(current, target_device_id)) current.push_back({target_device_id});
    return current;
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

    const bool verify_extended = input.virtual_display && is_extended(input.layout) &&
                                 input.configuration_option == ConfigurationOption::VerifyOnly;
    if (verify_extended) {
      result.device_preparation = DevicePreparation::VerifyOnly;
    } else if (input.virtual_display) {
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

    if (input.virtual_display && is_extended(input.layout) && !verify_extended) {
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

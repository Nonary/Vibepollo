#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace display_helper_integration::request_policy {
  enum class ConfigurationOption {
    Disabled,
    EnsureActive,
    EnsureOnlyDisplay,
  };

  enum class VirtualDisplayLayout {
    Exclusive,
    Extended,
    ExtendedPrimary,
    ExtendedIsolated,
    ExtendedPrimaryIsolated,
  };

  enum class DevicePreparation {
    EnsureActive,
    EnsurePrimary,
    EnsureOnlyDisplay,
  };

  struct Resolution {
    int width = 0;
    int height = 0;
  };

  struct Input {
    ConfigurationOption configuration_option {ConfigurationOption::Disabled};
    VirtualDisplayLayout layout {VirtualDisplayLayout::Exclusive};
    bool virtual_display = false;
    bool virtual_display_failed = false;
    bool physical_output_override = false;
    bool hdr_profile_selected = false;
    bool rtx_hdr_source_enabled = false;
    bool hdr_requested = false;
    std::string target_device_id;
    std::vector<std::vector<std::string>> topology_snapshot;
    std::optional<Resolution> remapped_resolution;
  };

  struct Result {
    bool dispatch = true;
    bool apply_hdr_profile_to_physical = false;
    std::optional<DevicePreparation> device_preparation;
    std::optional<bool> hdr_enabled;
    std::optional<Resolution> initial_resolution;
    std::optional<Resolution> applied_resolution;
    std::vector<std::vector<std::string>> topology;
  };

  [[nodiscard]] bool virtual_display_mutation_allowed(bool display_restore_in_progress);
  [[nodiscard]] bool supersede_restore_for_virtual_display(
    const std::function<void()> &disarm_restore,
    const std::function<bool()> &restore_in_progress
  );
  [[nodiscard]] Result evaluate(const Input &input);
}  // namespace display_helper_integration::request_policy

#pragma once

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
    bool physical_output_override = false;
    bool rtx_hdr_source_enabled = false;
    bool hdr_requested = false;
    std::string target_device_id;
    std::vector<std::vector<std::string>> topology_snapshot;
    std::optional<Resolution> remapped_resolution;
  };

  struct Result {
    bool dispatch = true;
    std::optional<DevicePreparation> device_preparation;
    std::optional<bool> hdr_enabled;
    std::optional<Resolution> initial_resolution;
    std::optional<Resolution> applied_resolution;
    std::vector<std::vector<std::string>> topology;
  };

  [[nodiscard]] Result evaluate(const Input &input);
}  // namespace display_helper_integration::request_policy

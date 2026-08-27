/**
 * @file src/platform/linux/kmsgrab_selection.h
 * @brief Pure helpers for naming and selecting Linux KMS outputs.
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace platf::kms::selection {

  inline bool driver_is_nvidia(std::string_view driver_name) {
    return driver_name.starts_with("nvidia-drm");
  }

  /**
   * @brief Whether a KMS card's scanout DMA-BUFs can use the CUDA import path.
   *
   * Vibeshine DRM is intentionally a display-only device. It forwards the
   * renderer GPU's imported DMA-BUF and modifier, so it must not be rejected
   * merely because its DRM driver is not nvidia-drm. Capture treats this path
   * as direct-import-only so a renderer-association regression fails closed.
   */
  inline bool driver_supports_cuda_import(std::string_view driver_name) {
    return driver_is_nvidia(driver_name) || driver_name == "vibeshine_drm";
  }

  inline bool driver_requires_direct_import(std::string_view driver_name) {
    return driver_name == "vibeshine_drm";
  }

  inline bool driver_supports_presentation_events(std::string_view driver_name) {
    return driver_name == "vibeshine_drm";
  }

  struct monitor_t {
    std::string card_path;
    std::string connector_name;
    std::uint32_t crtc_id;
    std::uint32_t monitor_index;
  };

  struct named_monitor_t {
    monitor_t monitor;
    std::string display_name;
  };

  inline std::optional<std::uint32_t> parse_numeric_alias(std::string_view value) {
    if (value.empty()) {
      return std::nullopt;
    }

    std::uint32_t result {};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc {} || end != value.data() + value.size()) {
      return std::nullopt;
    }

    return result;
  }

  inline bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
      return false;
    }

    return std::ranges::equal(left, right, [](char lhs, char rhs) {
      const auto lower = [](char value) {
        if (value >= 'A' && value <= 'Z') {
          return static_cast<char>(value + ('a' - 'A'));
        }
        return value;
      };
      return lower(lhs) == lower(rhs);
    });
  }

  inline std::vector<named_monitor_t> name_monitors(std::vector<monitor_t> monitors) {
    std::ranges::sort(monitors, {}, &monitor_t::monitor_index);

    std::unordered_map<std::string, std::size_t> connector_name_counts;
    for (const auto &monitor : monitors) {
      ++connector_name_counts[monitor.connector_name];
    }

    std::vector<named_monitor_t> named_monitors;
    named_monitors.reserve(monitors.size());
    for (auto &monitor : monitors) {
      auto display_name = monitor.connector_name;
      if (connector_name_counts[monitor.connector_name] > 1) {
        display_name = monitor.card_path + ':' + monitor.connector_name;
      }
      named_monitors.emplace_back(named_monitor_t {std::move(monitor), std::move(display_name)});
    }

    return named_monitors;
  }

  inline std::optional<monitor_t> resolve_named_monitor(
    std::string_view display_name,
    std::span<const named_monitor_t> named_monitors
  ) {
    // Prefer the exact public name, including the card qualifier used when two
    // GPUs expose connectors with the same DRM name.
    for (const auto &named_monitor : named_monitors) {
      if (ascii_iequals(display_name, named_monitor.display_name)) {
        return named_monitor.monitor;
      }
    }

    // Accept an unqualified connector name only when it identifies one output.
    // This helps configurations survive a GPU being added or removed without
    // ever guessing when two cards expose (for example) HDMI-A-1.
    const monitor_t *match = nullptr;
    for (const auto &named_monitor : named_monitors) {
      if (!ascii_iequals(display_name, named_monitor.monitor.connector_name)) {
        continue;
      }
      if (match) {
        return std::nullopt;
      }
      match = &named_monitor.monitor;
    }

    if (match) {
      return *match;
    }
    return std::nullopt;
  }

}  // namespace platf::kms::selection

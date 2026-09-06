/**
 * @file src/platform/linux/private_display_restore_policy.h
 * @brief Restore guard selection for Linux private streaming displays.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>

namespace platf::linux_private_display::restore_policy {
  /** Hotplug itself can change the desktop; capture before admitting the connection. */
  template <typename Configuration, typename Capture, typename Connect>
  bool connect_with_snapshot(std::optional<Configuration> &snapshot, Capture capture, Connect connect) {
    if (!snapshot) {
      snapshot = capture();
      if (!snapshot) {
        return false;
      }
    }
    return connect();
  }

  /** Activation checks allow retiring outputs; the final check also enforces saved disables. */
  template <typename Configuration>
  bool snapshot_matches(const Configuration &snapshot, const Configuration &current, const bool final = false) {
    const auto refresh = [](const auto &output) {
      const auto id = output.value("currentModeId", std::string {});
      for (const auto &mode : output.value("modes", Configuration::array())) {
        if (mode.value("id", std::string {}) == id) {
          return mode.value("refreshRate", 0.0);
        }
      }
      return 0.0;
    };
    return std::ranges::all_of(snapshot["outputs"], [&](const auto &saved) {
      const auto output = std::ranges::find_if(current["outputs"], [&](const auto &present) {
        return present.value("name", std::string {}) == saved.value("name", std::string {});
      });
      const bool active = output != current["outputs"].end() &&
                          output->value("connected", false) && output->value("enabled", false);
      if (!saved.value("enabled", false)) {
        return !final || !active;
      }
      if (!active) {
        return false;
      }
      const auto saved_mode = saved.value("currentModeId", std::string {});
      const bool exact_mode = saved_mode.empty() || output->value("currentModeId", std::string {}) == saved_mode;
      const bool equivalent_mode = saved.value("size", Configuration::object()) == output->value("size", Configuration::object()) &&
                                   std::abs(refresh(saved) - refresh(*output)) < 0.2;
      return (exact_mode || equivalent_mode) &&
             std::abs(saved.value("scale", 1.0) - output->value("scale", 1.0)) < 0.01 &&
             saved.value("pos", Configuration::object()) == output->value("pos", Configuration::object()) &&
             saved.value("rotation", 1) == output->value("rotation", 1) &&
             saved.value("priority", 0) == output->value("priority", 0) &&
             (!saved.contains("hdr") || saved.value("hdr", false) == output->value("hdr", false));
    });
  }

  struct candidate_t {
    // Candidates outlive the temporary names read while collecting outputs.
    // Own the identifier until guard selection and activation lookup finish.
    std::string name;
    bool enabled {false};
    bool connected {false};
    bool private_output {false};
    bool retiring {false};
  };

  /** Prefer a surviving physical output, falling back to a distinct surviving private output. */
  inline std::optional<std::string> select_guard(const std::span<const candidate_t> candidates) {
    const auto eligible = [](const candidate_t &candidate) {
      return !candidate.name.empty() && candidate.enabled && candidate.connected && !candidate.retiring;
    };
    const auto physical = std::ranges::find_if(candidates, [&](const auto &candidate) {
      return eligible(candidate) && !candidate.private_output;
    });
    if (physical != candidates.end()) {
      return std::string {physical->name};
    }
    const auto fallback = std::ranges::find_if(candidates, eligible);
    return fallback == candidates.end() ? std::nullopt : std::make_optional(std::string {fallback->name});
  }

  /** Resolve guard activation without allowing inconsistent compositor state to abort teardown. */
  template <typename ActivationMap>
  inline std::optional<typename ActivationMap::mapped_type> guard_activation(
    const std::optional<std::string> &guard_output,
    const ActivationMap &activation_by_output
  ) {
    if (!guard_output) {
      return std::nullopt;
    }
    const auto activation = activation_by_output.find(*guard_output);
    return activation == activation_by_output.end() ?
             std::nullopt :
             std::make_optional(activation->second);
  }

  /** Keep the last working private scanout across restart until a physical capture source exists. */
  constexpr bool preserve_private_scanout(
    const bool capture_ready_physical,
    const bool capture_ready_private
  ) {
    return !capture_ready_physical && capture_ready_private;
  }
}  // namespace platf::linux_private_display::restore_policy

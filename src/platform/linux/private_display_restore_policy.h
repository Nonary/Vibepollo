/**
 * @file src/platform/linux/private_display_restore_policy.h
 * @brief Restore guard selection for Linux private streaming displays.
 */
#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace platf::linux_private_display::restore_policy {
  struct candidate_t {
    std::string_view name;
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

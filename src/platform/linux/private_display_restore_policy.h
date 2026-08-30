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
  };

  /** An enabled saved topology must retain a scanout until one saved output is capture-ready. */
  inline bool requires_guard(const std::span<const candidate_t> candidates) {
    return std::ranges::any_of(candidates, [](const auto &candidate) {
      return candidate.enabled;
    });
  }

  /** Prefer a physical saved output, falling back to a connected private output for private-only baselines. */
  inline std::optional<std::string> select_guard(const std::span<const candidate_t> candidates) {
    const auto eligible = [](const candidate_t &candidate) {
      return !candidate.name.empty() && candidate.enabled && candidate.connected;
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

  /** Keep the last working private scanout across restart until a physical capture source exists. */
  constexpr bool preserve_private_scanout(
    const bool capture_ready_physical,
    const bool capture_ready_private
  ) {
    return !capture_ready_physical && capture_ready_private;
  }
}  // namespace platf::linux_private_display::restore_policy

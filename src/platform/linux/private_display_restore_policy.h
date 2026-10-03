/**
 * @file src/platform/linux/private_display_restore_policy.h
 * @brief Restore guard selection for Linux private streaming displays.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

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

  enum class connection_result_e {
    failed,
    published,
    publication_failed,
  };

  /**
   * After an acknowledged hotplug, failed publication is not evidence that the
   * connector is unused. The compositor may already have selected it as the
   * only live scanout. Retain the snapshot and admitted connector for guarded
   * restoration instead of performing an unverified rollback disconnect.
   */
  template <typename Configuration, typename Capture, typename Connect, typename Publish>
  connection_result_e connect_with_snapshot_and_publication(
    std::optional<Configuration> &snapshot, Capture capture, Connect connect, Publish publish
  ) {
    if (!connect_with_snapshot(snapshot, capture, connect)) return connection_result_e::failed;
    return publish() ? connection_result_e::published : connection_result_e::publication_failed;
  }

  /** Resolve a saved mode against the current catalog without trusting a reused identifier. */
  template <typename Configuration>
  std::optional<std::string> select_restore_mode(const Configuration &saved, const Configuration *present) {
    const auto saved_id = saved.value("currentModeId", std::string {});
    if (!present || saved_id.empty()) return std::nullopt;
    const auto current_modes = present->find("modes");
    if (current_modes == present->end() || !current_modes->is_array()) return std::nullopt;
    const auto size = [](const Configuration &output) -> std::optional<std::pair<int, int>> {
      const auto dimensions = output.find("size");
      if (dimensions == output.end() || !dimensions->is_object()) return std::nullopt;
      const auto width = dimensions->value("width", 0);
      const auto height = dimensions->value("height", 0);
      if (width <= 0 || height <= 0) return std::nullopt;
      return std::pair {width, height};
    };
    const auto refresh = [](const Configuration &mode) -> std::optional<double> {
      const auto rate = mode.value("refreshRate", 0.0);
      return std::isfinite(rate) && rate > 0.0 ? std::make_optional(rate) : std::nullopt;
    };
    auto saved_size = size(saved);
    std::optional<double> saved_refresh;
    if (const auto saved_modes = saved.find("modes"); saved_modes != saved.end() && saved_modes->is_array()) {
      for (const auto &mode : *saved_modes) {
        if (mode.value("id", std::string {}) != saved_id) continue;
        // Output geometry can be rotated or scaled. The mode catalog contains
        // the pixel dimensions to compare with candidate modes before rotation.
        if (const auto pixels = size(mode)) saved_size = pixels;
        saved_refresh = refresh(mode);
        break;
      }
    }
    const auto matches = [&](const Configuration &mode) {
      const auto pixels = size(mode);
      const auto rate = refresh(mode);
      return (!saved_size || (pixels && *pixels == *saved_size)) &&
             (!saved_refresh || (rate && std::abs(*rate - *saved_refresh) < 0.2));
    };
    for (const auto &mode : *current_modes) {
      if (mode.value("id", std::string {}) == saved_id && matches(mode)) return saved_id;
    }
    // A different identifier requires complete semantic evidence; unknown
    // refresh or dimensions cannot establish equivalence on their own.
    if (!saved_size || !saved_refresh) return std::nullopt;
    for (const auto &mode : *current_modes) {
      const auto id = mode.value("id", std::string {});
      if (!id.empty() && matches(mode)) return id;
    }
    return std::nullopt;
  }

  /** Only a desired output published by both KScreen and capture can survive retirement. */
  template <typename Configuration>
  std::optional<std::string> select_capture_ready_guard(
    const Configuration &current,
    const std::set<std::string> &desired_outputs,
    std::span<const std::string> capture_outputs
  ) {
    for (const auto &output : current["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (desired_outputs.contains(name) && output.value("connected", false) && output.value("enabled", false) &&
          std::ranges::find(capture_outputs, name) != capture_outputs.end()) {
        return name;
      }
    }
    return std::nullopt;
  }

  /** Revalidate a published survivor immediately before disabling an active scanout. */
  template <typename FindGuard, typename GuardActive, typename Allowed, typename Retire>
  bool retire_with_capture_guard(
    bool retiring_active_scanout, FindGuard find_guard, GuardActive guard_active, Allowed allowed, Retire retire
  ) {
    if (!allowed()) return false;
    if (retiring_active_scanout) {
      const auto guard = find_guard();
      if (!guard || !allowed() || !guard_active(*guard)) return false;
    }
    return allowed() && retire();
  }

  /**
   * Activation checks allow retiring outputs. KWin renumbers priorities while
   * a retiring private output still holds a slot; enforce saved priorities and
   * disables only after connector retirement, when the output set has settled.
   */
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
      const auto saved_size = saved.value("size", Configuration::object());
      const bool known_size = saved_size.is_object() && saved_size.value("width", 0) > 0 && saved_size.value("height", 0) > 0;
      const auto saved_refresh = refresh(saved);
      const auto current_refresh = refresh(*output);
      const bool known_refresh = std::isfinite(saved_refresh) && saved_refresh > 0.0;
      const bool size_matches = !known_size || saved_size == output->value("size", Configuration::object());
      const bool refresh_matches = !known_refresh ||
                                   (std::isfinite(current_refresh) && current_refresh > 0.0 && std::abs(saved_refresh - current_refresh) < 0.2);
      const bool exact_mode = !saved_mode.empty() && output->value("currentModeId", std::string {}) == saved_mode;
      const bool equivalent_mode = known_size && known_refresh && size_matches && refresh_matches;
      // Equal identifiers cannot bypass saved semantic evidence. When it is
      // incomplete, require the identifier and every available constraint.
      return (exact_mode || equivalent_mode) && size_matches && refresh_matches &&
             std::abs(saved.value("scale", 1.0) - output->value("scale", 1.0)) < 0.01 &&
             saved.value("pos", Configuration::object()) == output->value("pos", Configuration::object()) &&
             saved.value("rotation", 1) == output->value("rotation", 1) &&
             (!final || saved.value("priority", 0) == output->value("priority", 0)) &&
             (!saved.contains("hdr") || saved.value("hdr", false) == output->value("hdr", false));
    });
  }

  /** Released client reservations must not hide a still-connected private output. */
  template <typename Configuration>
  std::set<std::string> retiring_outputs(
    const Configuration &snapshot,
    const Configuration &current,
    const std::set<std::string> &managed_outputs,
    std::set<std::string> reserved_outputs
  ) {
    // Existing physical/dummy outputs can be configured as capture targets,
    // but only driver-managed connectors can be unplugged by this host.
    std::erase_if(reserved_outputs, [&](const auto &name) { return !managed_outputs.contains(name); });
    for (const auto &output : current["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (managed_outputs.contains(name) && output.value("connected", false)) {
        reserved_outputs.insert(name);
      }
    }
    // A private desktop that was already active before streaming is part of
    // the baseline, not a leaked streaming connector to be disconnected.
    for (const auto &saved : snapshot["outputs"]) {
      if (saved.value("enabled", false)) {
        reserved_outputs.erase(saved.value("name", std::string {}));
      }
    }
    return reserved_outputs;
  }

  /** A missing secondary monitor cannot block retirement behind a verified guard. */
  template <typename Configuration>
  Configuration connected_activation_snapshot(const Configuration &snapshot, const Configuration &current) {
    auto available = snapshot;
    auto &outputs = available["outputs"];
    outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&](const auto &saved) {
      if (!saved.value("enabled", false)) return false;
      return std::ranges::none_of(current["outputs"], [&](const auto &present) {
        return present.value("name", std::string {}) == saved.value("name", std::string {}) &&
               present.value("connected", false);
      });
    }), outputs.end());
    return available;
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

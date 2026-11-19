/**
 * @file src/platform/linux/private_display_capture_policy.h
 * @brief Pure capture-routing policy for Linux private displays.
 */
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platf::linux_private_display_capture {
  // Duplicate connector names occur across GPUs even for unplugged ports.
  // An unreadable status leaves the identity ambiguous.
  inline std::filesystem::path unique_connected_connector(std::span<const std::filesystem::path> candidates) {
    std::filesystem::path selected;
    for (const auto &path : candidates) {
      std::ifstream status {path / "status"};
      std::string state;
      if (!(status >> state)) return {};
      if (state == "disconnected") continue;
      if (state != "connected" || !selected.empty()) return {};
      selected = path;
    }
    return selected;
  }

  // KScreen reservations use names shared by multiple GPUs. An exclusive stream
  // keeps the DRM card it first reserved until the last owner releases that name.
  class reserved_connector_t {
  public:
    void forget_if_released(const std::map<std::string, std::string> &reservations) {
      if (!path_.empty() && std::ranges::none_of(reservations, [&](const auto &entry) { return entry.second == output_; })) {
        path_.clear();
      }
    }

    [[nodiscard]] bool bound_to(const std::string &output) const {
      return !path_.empty() && output_ == output;
    }

    [[nodiscard]] std::optional<bool> connection_status() const {
      std::ifstream status {path_ / "status"};
      std::string value;
      if (status >> value) {
        if (value == "connected") return true;
        if (value == "disconnected") return false;
      }
      return std::nullopt;
    }

    template<class Lookup>
    [[nodiscard]] std::optional<std::string> bind(const std::string &output, Lookup lookup) {
      if (!bound_to(output)) {
        std::filesystem::path selected = lookup(output);
        if (selected.empty()) return std::nullopt;
        path_ = std::move(selected);
        output_ = output;
      }
      const auto entry = path_.filename().string();
      return entry.substr(0, entry.find('-')) + ':' + output;
    }

  private:
    std::string output_;
    std::filesystem::path path_;
  };

  // Portal labels can be inferred from geometry. Keep their public order, but
  // open only a unique DRM connector, never a stream guessed from its bounds.
  inline std::vector<std::string> qualify_portal_switch_targets(
    std::vector<std::string> labels, std::span<const std::string> kms_targets
  ) {
    for (auto &label : labels) {
      const std::string suffix = ':' + label;
      const std::string *match = nullptr;
      for (const auto &target : kms_targets) {
        if (!target.ends_with(suffix)) continue;
        if (match) {
          match = nullptr;
          break;
        }
        match = &target;
      }
      label = match ? *match : "";
    }
    return labels;
  }

  /** Several clients can share one connector. An independently reserved peer needs a composed desktop. */
  [[nodiscard]] inline bool exclusive_output(
    bool requested, std::string_view output, const std::map<std::string, std::string> &reservations
  ) {
    return requested && !reservations.empty() &&
           std::ranges::all_of(reservations, [&](const auto &entry) { return entry.second == output; });
  }

  /** An explicit KMS backend must survive a temporarily dormant scanout. */
  [[nodiscard]] constexpr bool enable_kms(bool explicitly_requested, bool outputs_available) noexcept {
    return explicitly_requested || outputs_available;
  }

  /** Automatic private-display capture uses KMS; explicit choices are honored. */
  [[nodiscard]] constexpr bool prefer_kms(bool automatic_capture, bool private_output) noexcept {
    return automatic_capture && private_output;
  }

  /** Avoid unattended portal consent once a direct or native backend is selected. */
  [[nodiscard]] constexpr bool probe_portal(
    bool automatic_capture,
    bool explicitly_requested,
    bool native_compositor_selected,
    bool kms_selected
  ) noexcept {
    return explicitly_requested || (automatic_capture && !native_compositor_selected && !kms_selected);
  }

  /** Keep CAP_SYS_ADMIN permitted only when a later automatic private KMS route needs it. */
  [[nodiscard]] constexpr bool retain_kms_capability(bool automatic_private_pool_available) noexcept {
    return automatic_private_pool_available;
  }

  /** Startup may use dummy compositor names only when KMS capability will be discarded. */
  [[nodiscard]] constexpr bool use_dummy_compositor_names(
    bool elevated_privileges,
    bool retain_kms_capability
  ) noexcept {
    return elevated_privileges && !retain_kms_capability;
  }
}  // namespace platf::linux_private_display_capture

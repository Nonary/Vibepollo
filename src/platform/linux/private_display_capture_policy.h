/**
 * @file src/platform/linux/private_display_capture_policy.h
 * @brief Pure capture-routing policy for Linux private displays.
 */
#pragma once

namespace platf::linux_private_display_capture {
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

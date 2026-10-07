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

  /** Direct KMS is required to preserve managed private-display HDR scanout. */
  [[nodiscard]] constexpr bool prefer_kms(
    int dynamic_range,
    bool force_sdr,
    bool prefer_sdr_10bit,
    bool private_output
  ) noexcept {
    return dynamic_range > 0 && !force_sdr && !prefer_sdr_10bit && private_output;
  }

  /** Keep CAP_SYS_ADMIN permitted only when a later private HDR KMS route needs it. */
  [[nodiscard]] constexpr bool retain_kms_capability(bool private_hdr_pool_available) noexcept {
    return private_hdr_pool_available;
  }

  /** Startup may use dummy compositor names only when KMS capability will be discarded. */
  [[nodiscard]] constexpr bool use_dummy_compositor_names(
    bool elevated_privileges,
    bool retain_kms_capability
  ) noexcept {
    return elevated_privileges && !retain_kms_capability;
  }
}  // namespace platf::linux_private_display_capture

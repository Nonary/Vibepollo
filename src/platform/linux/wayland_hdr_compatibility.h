/**
 * @file src/platform/linux/wayland_hdr_compatibility.h
 * @brief Resolved-session gate for the optional native Wayland HDR launch policy.
 */
#pragma once

#include <cstdlib>
#include <string_view>

namespace platf::wayland_hdr_compatibility {
  enum class suppression_reason_e {
    none,
    disabled,
    not_wayland,
    resolved_sdr,
  };

  struct decision_t {
    bool enabled {};
    suppression_reason_e suppression_reason {suppression_reason_e::disabled};
  };

  /**
   * The machine-scoped host deliberately runs with neither WAYLAND_DISPLAY nor
   * DISPLAY because it captures through KMS and delegates desktop launches to
   * the selected session. In that architecture, the trusted controller must
   * describe the selected session separately from the host process's own
   * window-system access.
   */
  constexpr bool selected_session_is_wayland(
    bool process_uses_wayland,
    std::string_view managed_session_type
  ) {
    return process_uses_wayland || managed_session_type == "wayland";
  }

  inline bool selected_session_is_wayland(bool process_uses_wayland) {
    const char *managed_session_type = std::getenv("VIBEPOLLO_SESSION_TYPE");
    return selected_session_is_wayland(
      process_uses_wayland,
      managed_session_type ? std::string_view {managed_session_type} : std::string_view {}
    );
  }

  /**
   * The caller supplies the already-resolved stream mode. In particular, a
   * 10-bit SDR stream and a requested-but-unavailable HDR output are both
   * resolved_sdr: neither is evidence that a game should expose HDR.
   */
  constexpr decision_t resolve(
    bool configured,
    bool wayland_session,
    bool resolved_hdr
  ) {
    if (!configured) {
      return {.enabled = false, .suppression_reason = suppression_reason_e::disabled};
    }
    if (!wayland_session) {
      return {.enabled = false, .suppression_reason = suppression_reason_e::not_wayland};
    }
    if (!resolved_hdr) {
      return {.enabled = false, .suppression_reason = suppression_reason_e::resolved_sdr};
    }
    return {.enabled = true, .suppression_reason = suppression_reason_e::none};
  }
}  // namespace platf::wayland_hdr_compatibility

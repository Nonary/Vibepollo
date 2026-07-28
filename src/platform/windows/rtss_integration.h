/**
 * @file src/platform/windows/rtss_integration.h
 * @brief Windows-only RTSS frame limit integration via RTSSHooks.dll and Sunshine-managed profile.
 */
#pragma once

#ifdef _WIN32

  #include <optional>
  #include <string>

namespace platf {
  enum class rtss_apply_result {
    safe_to_fallback,
    applied,
    retained_exclusive,
  };

  enum class rtss_recovery_audit_result {
    clear,
    retained_exclusive,
  };

  struct rtss_status_t {
    bool enabled;  // frame limiter toggle
    bool path_configured;  // install_path not empty
    std::string configured_path;  // raw config value (may be relative)
    std::string resolved_path;  // absolute resolved path we will use
    bool path_exists;  // resolved path exists on disk
    bool hooks_found;  // RTSSHooks64.dll or RTSSHooks.dll exists
    bool profile_found;  // Sunshine-managed profile exists
    bool can_bootstrap_profile;  // Sunshine can create its profile automatically
    bool process_running;  // RTSS process currently running
  };

  // Apply an RTSS frame limit represented as numerator / denominator. A
  // retained_exclusive result means RTSS may still be limiting, but the
  // requested rate was not confirmed, so another limiter must not be enabled.
  rtss_apply_result rtss_streaming_start(int numerator, int denominator);

  // Re-apply RTSS frame limit and related settings without resetting originals.
  rtss_apply_result rtss_streaming_refresh(int numerator, int denominator);

  // Resolve any durable RTSS recovery snapshot before another limiter is
  // enabled. retained_exclusive means the caller must not select that limiter.
  rtss_recovery_audit_result rtss_audit_pending_recovery();

  // True when the most recent stream start/refresh abandoned RTSS hooks after
  // the response deadline. Callers may select another provider only when the
  // associated apply result is safe_to_fallback.
  bool rtss_hooks_stalled();

  // Restore any RTSS settings modified at stream start.
  // If keep_process_running is true, Sunshine leaves RTSS running for pause/resume scenarios.
  void rtss_streaming_stop(bool keep_process_running = false);

  void rtss_restore_pending_overrides();

  bool rtss_is_configured();

  // Query RTSS availability and installation status (no side effects).
  rtss_status_t rtss_get_status();

  void rtss_set_sync_limiter_override(std::optional<std::string> value);
  std::optional<std::string> rtss_get_sync_limiter_override();

  // Ensure RTSS is running ahead of game launch so its hooks attach before the process starts.
  bool rtss_warmup_process();
}  // namespace platf

#endif  // _WIN32

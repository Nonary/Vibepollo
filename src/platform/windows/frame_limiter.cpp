/**
 * @file src/platform/windows/frame_limiter.cpp
 * @brief Frame limiter provider selection and orchestration.
 */

#ifdef _WIN32

  #include "frame_limiter.h"

  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/misc.h"

  #include <algorithm>
  #include <array>
  #include <cctype>
  #include <cmath>
  #include <cstdint>
  #include <mutex>
  #include <numeric>
  #include <optional>
  #include <numeric>
  #include <string>
  #include <vector>

namespace platf {

  namespace {

    struct frame_limit_t {
      std::uint32_t millihz = 0;
      int rtss_numerator = 0;
      int rtss_denominator = 1;
      int nvcp_fps = 0;

      [[nodiscard]] double fps() const {
        return rtss_denominator > 0 ? static_cast<double>(rtss_numerator) / rtss_denominator : 0.0;
      }

      [[nodiscard]] bool is_fractional() const {
        return millihz > 0 && millihz % 1000 != 0;
      }
    };

    frame_limit_t make_frame_limit(std::uint32_t millihz) {
      if (millihz == 0) {
        return {};
      }

      constexpr std::uint32_t kMillihzPerHertz = 1000;
      constexpr std::uint32_t kMaxFrameLimitMillihz = 1'000'000;
      // Network-provided FPS can be much larger than the UI/configuration
      // range. Clamp before reducing/casting to RTSS's signed integer fields.
      millihz = std::min(millihz, kMaxFrameLimitMillihz);
      const auto divisor = std::gcd(millihz, kMillihzPerHertz);
      return {
        .millihz = millihz,
        .rtss_numerator = static_cast<int>(millihz / divisor),
        .rtss_denominator = static_cast<int>(kMillihzPerHertz / divisor),
        .nvcp_fps = std::max(1, framegen::rounded_fps_from_millihz(millihz)),
      };
    }

    std::uint32_t integer_fps_to_millihz(int fps) {
      return fps > 0 ? framegen::saturating_refresh_millihz(static_cast<std::uint32_t>(fps), 1000) : 0;
    }

    void log_nvcp_fractional_rounding(const frame_limit_t &limit) {
      if (limit.is_fractional()) {
        BOOST_LOG(warning) << "NVIDIA Control Panel only supports whole-number frame limits; rounding "
                           << limit.fps() << " FPS to " << limit.nvcp_fps << " FPS.";
      }
    }

    frame_limiter_provider g_active_provider = frame_limiter_provider::none;
    std::mutex g_lifecycle_mutex;
    std::uint8_t g_stream_owner_mask = 0;
    bool g_nvcp_started = false;
    bool g_rtss_cleanup_needed = false;
    bool g_nvcp_force_vsync_off = false;
    bool g_nvcp_apply_smooth_motion = false;
    bool g_gen1_framegen_fix_active = false;
    bool g_gen2_framegen_fix_active = false;
    bool g_stream_policy_overrides_active = false;
    frame_limit_t g_last_effective_limit;
    bool g_prev_frame_limiter_enabled = false;
    std::string g_prev_frame_limiter_provider;
    bool g_prev_frame_limiter_provider_set = false;
    bool g_prev_disable_vsync = false;
    std::string g_prev_rtss_frame_limit_type;
    bool g_prev_rtss_frame_limit_type_set = false;
    std::string g_prev_capture_mode;
    bool g_prev_capture_mode_set = false;

    const char *frame_limiter_owner_to_string(frame_limiter_owner owner) {
      switch (owner) {
        case frame_limiter_owner::rtsp:
          return "rtsp";
        case frame_limiter_owner::webrtc:
          return "webrtc";
        default:
          return "unknown";
      }
    }

    frame_limiter_provider parse_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (ch == '-' || ch == '_' || ch == ' ') {
          continue;
        }
        normalized.push_back((char) std::tolower(static_cast<unsigned char>(ch)));
      }
      if (normalized.empty() || normalized == "auto") {
        return frame_limiter_provider::auto_detect;
      }
      if (normalized == "rtss") {
        return frame_limiter_provider::rtss;
      }
      if (normalized == "nvidiacontrolpanel" || normalized == "nvidia" || normalized == "nvcp") {
        return frame_limiter_provider::nvidia_control_panel;
      }
      if (normalized == "none" || normalized == "disabled") {
        return frame_limiter_provider::none;
      }
      return frame_limiter_provider::auto_detect;
    }

    bool provider_available(frame_limiter_provider provider) {
      switch (provider) {
        case frame_limiter_provider::nvidia_control_panel:
          return frame_limiter_nvcp::is_available();
        case frame_limiter_provider::rtss:
          return rtss_is_configured();
        default:
          return false;
      }
    }

    bool has_amd_gpu() {
      for (const auto &gpu : enumerate_gpus()) {
        if (gpu.vendor_id == 0x1002 || gpu.vendor_id == 0x1022) {
          return true;
        }
      }
      return false;
    }

    std::string normalize_frame_generation_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          normalized.push_back((char) std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      return normalized;
    }

    std::string select_framegen_sync_limiter(const framegen::stream_start_policy_t &policy, bool nvidia_gpu_present, bool amd_gpu_present) {
      // Smooth Motion needs RTSS to pace the generated frames from their front edge. Do not
      // let the generic NVIDIA virtual-display policy select Reflex for this provider.
      if (policy.smooth_motion) {
        return "front edge sync";
      }
      if (policy.auto_virtual_framegen_limiter && nvidia_gpu_present && !amd_gpu_present) {
        return "nvidia reflex";
      }
      const auto normalized_provider = normalize_frame_generation_provider(policy.frame_generation_provider);
      const bool game_provided_framegen = normalized_provider == "gameprovided";
      if (game_provided_framegen && nvidia_gpu_present && !amd_gpu_present) {
        return "nvidia reflex";
      }
      return "front edge sync";
    }

  }  // namespace

  const char *frame_limiter_provider_to_string(frame_limiter_provider provider) {
    switch (provider) {
      case frame_limiter_provider::none:
        return "none";
      case frame_limiter_provider::auto_detect:
        return "auto";
      case frame_limiter_provider::rtss:
        return "rtss";
      case frame_limiter_provider::nvidia_control_panel:
        return "nvidia-control-panel";
      default:
        return "unknown";
    }
  }

  void frame_limiter_streaming_start(
    frame_limiter_owner owner,
    const framegen::stream_start_policy_t &policy
  ) {
    std::scoped_lock lock {g_lifecycle_mutex};
    const auto owner_bit = static_cast<std::uint8_t>(owner);
    if ((g_stream_owner_mask & owner_bit) != 0) {
      BOOST_LOG(debug) << "Frame limiter start ignored for existing " << frame_limiter_owner_to_string(owner) << " owner.";
      return;
    }
    if (g_stream_owner_mask != 0) {
      g_stream_owner_mask |= owner_bit;
      BOOST_LOG(debug) << "Frame limiter " << frame_limiter_owner_to_string(owner)
                       << " owner joined existing stream overrides (owner mask="
                       << static_cast<unsigned int>(g_stream_owner_mask) << ").";
      return;
    }
    g_stream_owner_mask = owner_bit;

    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
    g_rtss_cleanup_needed = false;
    g_gen1_framegen_fix_active = policy.capture_fix_enabled;
    g_gen2_framegen_fix_active = false;

    const bool auto_framegen_policy_enabled = policy.auto_virtual_framegen_limiter;
    const bool capture_fix_enabled = policy.capture_fix_enabled;
    const bool physical_framegen_policy_enabled = policy.physical_framegen_capture;
    const bool policy_overrides_enabled = capture_fix_enabled || auto_framegen_policy_enabled || physical_framegen_policy_enabled;
    const bool frame_limit_enabled = config::frame_limiter.enable || policy_overrides_enabled || (policy.lossless_rtss_limit && *policy.lossless_rtss_limit > 0);
    const bool nvidia_gpu_present = platf::has_nvidia_gpu();
    const bool amd_gpu_present = has_amd_gpu();
    const bool nvcp_ready = frame_limiter_nvcp::is_available();
    const bool want_smooth_motion = policy.smooth_motion && nvidia_gpu_present;

    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");
    const bool rtss_sync_overridden = config::has_runtime_config_override("rtss_frame_limit_type");
    const auto configured_provider = parse_provider(config::frame_limiter.provider);
    const bool allow_framegen_default_provider = !provider_overridden && configured_provider == frame_limiter_provider::auto_detect;
    const bool default_policy_can_use_rtss =
      configured_provider == frame_limiter_provider::auto_detect || configured_provider == frame_limiter_provider::rtss;

    // Frame generation policy: enable limiter/vsync defaults and tune RTSS sync unless explicitly overridden.
    if (policy_overrides_enabled) {
      g_prev_frame_limiter_enabled = config::frame_limiter.enable;
      g_prev_frame_limiter_provider = config::frame_limiter.provider;
      g_prev_frame_limiter_provider_set = true;
      g_prev_disable_vsync = config::frame_limiter.disable_vsync;
      config::frame_limiter.enable = true;
      config::frame_limiter.disable_vsync = true;
      if ((capture_fix_enabled || physical_framegen_policy_enabled) && allow_framegen_default_provider) {
        config::frame_limiter.provider = "rtss";
      }
      if (default_policy_can_use_rtss) {
        if (!rtss_sync_overridden) {
          g_prev_rtss_frame_limit_type = config::rtss.frame_limit_type;
          g_prev_rtss_frame_limit_type_set = true;
          config::rtss.frame_limit_type = select_framegen_sync_limiter(policy, nvidia_gpu_present, amd_gpu_present);
        } else {
          g_prev_rtss_frame_limit_type_set = false;
        }
      } else {
        g_prev_rtss_frame_limit_type_set = false;
      }
    } else {
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }
    g_stream_policy_overrides_active = policy_overrides_enabled;

    if (policy.requires_virtual_display && policy.effective_wgc_capture && !config::video.capture.starts_with("wgc")) {
      g_prev_capture_mode = config::video.capture;
      g_prev_capture_mode_set = true;
      config::video.capture = "wgc";
    } else if (policy.physical_framegen_capture && config::video.capture.empty()) {
      g_prev_capture_mode = config::video.capture;
      g_prev_capture_mode_set = true;
      config::video.capture = "ddx";
    } else {
      g_prev_capture_mode_set = false;
    }

    const bool want_nv_vsync_override = (config::frame_limiter.disable_vsync || policy_overrides_enabled) && nvidia_gpu_present && nvcp_ready;
    g_nvcp_force_vsync_off = want_nv_vsync_override;
    g_nvcp_apply_smooth_motion = want_smooth_motion;

    bool nvcp_already_invoked = false;
    std::uint32_t effective_limit_millihz = policy.frame_limit_millihz > 0 ?
                                                policy.frame_limit_millihz :
                                                (policy.fps_scaled > 0 ?
                                                   static_cast<std::uint32_t>(policy.fps_scaled) :
                                                   framegen::normalize_refresh_millihz(policy.fps));
    if (policy.lossless_rtss_limit && *policy.lossless_rtss_limit > 0) {
      effective_limit_millihz = integer_fps_to_millihz(*policy.lossless_rtss_limit);
    }
    if (config::frame_limiter.fps_limit_millihz > 0) {
      effective_limit_millihz = config::frame_limiter.fps_limit_millihz;
    }
    const auto effective_limit = make_frame_limit(effective_limit_millihz);
    g_last_effective_limit = effective_limit;

    BOOST_LOG(debug) << "Frame limiter decision: configured_provider="
                     << frame_limiter_provider_to_string(parse_provider(config::frame_limiter.provider))
                     << " effective_limit=" << effective_limit.fps()
                     << " effective_limit_millihz=" << effective_limit.millihz
                     << " capture_fix=" << capture_fix_enabled
                     << " lossless_rtss_limit=" << (policy.lossless_rtss_limit ? *policy.lossless_rtss_limit : 0)
                     << " frame_generation_provider=" << policy.frame_generation_provider
                     << " frame_generation_enabled=" << policy.frame_generation_enabled
                     << " uses_virtual_display=" << policy.uses_virtual_display
                     << " effective_wgc_capture=" << policy.effective_wgc_capture
                     << " physical_framegen_capture=" << policy.physical_framegen_capture
                     << " auto_virtual_framegen_limiter=" << policy.auto_virtual_framegen_limiter
                     << " nvidia_gpu=" << nvidia_gpu_present
                     << " amd_gpu=" << amd_gpu_present
                     << " nvcp_ready=" << nvcp_ready
                     << " want_nv_vsync_override=" << want_nv_vsync_override
                     << " want_smooth_motion=" << want_smooth_motion;

    if (frame_limit_enabled) {
      auto configured = parse_provider(config::frame_limiter.provider);
      std::vector<frame_limiter_provider> order;

      switch (configured) {
        case frame_limiter_provider::none:
          break;
        case frame_limiter_provider::auto_detect:
          order = {frame_limiter_provider::rtss, frame_limiter_provider::nvidia_control_panel};
          break;
        case frame_limiter_provider::rtss:
          // Preserve an explicit RTSS preference, but allow the NVIDIA driver
          // to rescue stream startup when RTSS is installed but unresponsive.
          order = {frame_limiter_provider::rtss, frame_limiter_provider::nvidia_control_panel};
          break;
        default:
          order = {configured};
          break;
      }

      bool applied = false;
      for (auto provider : order) {
        // RTSS start also audits its durable recovery snapshot. Always enter
        // that path so a broken or removed RTSS install cannot bypass an
        // unresolved prior mutation and hand the stream to another limiter.
        if (provider != frame_limiter_provider::rtss && !provider_available(provider)) {
          BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                             << "' not available";
          if (configured != frame_limiter_provider::auto_detect) {
            break;
          }
          continue;
        }
        if (provider != frame_limiter_provider::rtss) {
          // Recovery may launch RTSS solely to drive its hooks. Always retain
          // lifecycle cleanup even when the audit succeeds and another
          // provider is selected.
          g_rtss_cleanup_needed = true;
          if (rtss_audit_pending_recovery() == rtss_recovery_audit_result::retained_exclusive) {
            g_active_provider = frame_limiter_provider::rtss;
            applied = true;
            BOOST_LOG(warning) << "RTSS retained exclusive frame-limiter ownership because its state could not be resolved; "
                                  "the configured provider was skipped";
            break;
          }
        }

        if (provider == frame_limiter_provider::nvidia_control_panel) {
          log_nvcp_fractional_rounding(effective_limit);
          bool ok = frame_limiter_nvcp::streaming_start(
            effective_limit.nvcp_fps,
            true,
            false,
            want_nv_vsync_override,
            false,
            want_smooth_motion
          );
          if (ok) {
            g_active_provider = frame_limiter_provider::nvidia_control_panel;
            applied = true;
            nvcp_already_invoked = true;
            BOOST_LOG(info) << "Frame limiter provider 'nvidia-control-panel' applied";
            break;
          }
        } else if (provider == frame_limiter_provider::rtss) {
          g_rtss_cleanup_needed = true;
          const auto result =
            rtss_streaming_start(effective_limit.rtss_numerator, effective_limit.rtss_denominator);
          if (result != rtss_apply_result::safe_to_fallback) {
            g_active_provider = frame_limiter_provider::rtss;
            applied = true;
            if (result == rtss_apply_result::applied) {
              BOOST_LOG(info) << "Frame limiter provider 'rtss' applied";
            } else {
              BOOST_LOG(warning) << "RTSS retained exclusive frame-limiter ownership, but the requested limit was not confirmed";
            }
            break;
          }
        }

        BOOST_LOG(warning) << "Frame limiter provider '" << frame_limiter_provider_to_string(provider)
                           << "' failed to apply limit";
        const bool allow_stall_fallback = configured == frame_limiter_provider::rtss &&
                                          provider == frame_limiter_provider::rtss &&
                                          rtss_hooks_stalled();
        if (allow_stall_fallback) {
          BOOST_LOG(warning) << "RTSS is stalled; falling back to the NVIDIA driver frame limiter";
        } else if (configured != frame_limiter_provider::auto_detect) {
          break;
        }
      }

      if (!applied && configured != frame_limiter_provider::none) {
        BOOST_LOG(warning) << "Frame limiter enabled but no provider applied";
      }
    }

    const bool want_disable_nv_frame_limit = g_active_provider == frame_limiter_provider::rtss && nvidia_gpu_present && nvcp_ready;

    if ((want_disable_nv_frame_limit || want_nv_vsync_override || want_smooth_motion) && !nvcp_already_invoked) {
      bool nvcp_result = frame_limiter_nvcp::streaming_start(
        effective_limit.nvcp_fps,
        false,
        want_disable_nv_frame_limit,
        want_nv_vsync_override,
        false,
        want_smooth_motion
      );
      nvcp_already_invoked = true;
      if (want_smooth_motion && !nvcp_result) {
        BOOST_LOG(warning) << "Requested NVIDIA Smooth Motion but NVIDIA Control Panel overrides failed";
      }
    }

    if (nvcp_already_invoked) {
      g_nvcp_started = true;
    }
  }

  bool frame_limiter_prepare_launch(const framegen::stream_start_policy_t &policy) {
    const bool capture_fix_enabled = policy.capture_fix_enabled;
    const bool auto_framegen_policy_enabled = policy.auto_virtual_framegen_limiter;
    const bool physical_framegen_policy_enabled = policy.physical_framegen_capture;
    const bool frame_limit_enabled = config::frame_limiter.enable || capture_fix_enabled || auto_framegen_policy_enabled || physical_framegen_policy_enabled || (policy.lossless_rtss_limit && *policy.lossless_rtss_limit > 0);
    if (!frame_limit_enabled) {
      return false;
    }

    const bool rtss_available = rtss_is_configured();
    bool want_rtss = false;
    const bool provider_overridden = config::has_runtime_config_override("frame_limiter_provider");

    if (capture_fix_enabled || auto_framegen_policy_enabled || physical_framegen_policy_enabled) {
      if (provider_overridden) {
        auto configured = parse_provider(config::frame_limiter.provider);
        switch (configured) {
          case frame_limiter_provider::rtss:
          case frame_limiter_provider::auto_detect:
            want_rtss = rtss_available;
            break;
          default:
            want_rtss = false;
            break;
        }
      } else {
        want_rtss = rtss_available;
      }
    } else {
      auto configured = parse_provider(config::frame_limiter.provider);
      switch (configured) {
        case frame_limiter_provider::rtss:
          want_rtss = rtss_available;
          break;
        case frame_limiter_provider::auto_detect:
          want_rtss = rtss_available;
          break;
        default:
          want_rtss = false;
          break;
      }
    }

    if (!want_rtss) {
      return false;
    }

    return rtss_warmup_process();
  }

  void frame_limiter_streaming_stop(
    frame_limiter_owner owner,
    bool keep_rtss_running
  ) {
    std::scoped_lock lock {g_lifecycle_mutex};
    const auto owner_bit = static_cast<std::uint8_t>(owner);
    if ((g_stream_owner_mask & owner_bit) == 0) {
      return;
    }
    g_stream_owner_mask &= static_cast<std::uint8_t>(~owner_bit);
    if (g_stream_owner_mask != 0) {
      BOOST_LOG(debug) << "Frame limiter stop deferred after releasing " << frame_limiter_owner_to_string(owner)
                       << "; remaining owner mask=" << static_cast<unsigned int>(g_stream_owner_mask) << ".";
      return;
    }

    if (g_stream_policy_overrides_active) {
      config::frame_limiter.enable = g_prev_frame_limiter_enabled;
      if (g_prev_frame_limiter_provider_set) {
        config::frame_limiter.provider = g_prev_frame_limiter_provider;
      }
      config::frame_limiter.disable_vsync = g_prev_disable_vsync;
      if (g_prev_rtss_frame_limit_type_set) {
        config::rtss.frame_limit_type = g_prev_rtss_frame_limit_type;
      }
      g_gen1_framegen_fix_active = false;
      g_gen2_framegen_fix_active = false;
      g_stream_policy_overrides_active = false;
      g_prev_frame_limiter_provider_set = false;
      g_prev_rtss_frame_limit_type_set = false;
    }

    if (g_prev_capture_mode_set) {
      config::video.capture = g_prev_capture_mode;
      g_prev_capture_mode.clear();
      g_prev_capture_mode_set = false;
    }

    if (g_rtss_cleanup_needed) {
      rtss_streaming_stop(keep_rtss_running);
    }

    if (g_nvcp_started || g_active_provider == frame_limiter_provider::nvidia_control_panel) {
      frame_limiter_nvcp::streaming_stop();
    }

    g_active_provider = frame_limiter_provider::none;
    g_nvcp_started = false;
    g_rtss_cleanup_needed = false;
    g_nvcp_force_vsync_off = false;
    g_nvcp_apply_smooth_motion = false;
    g_last_effective_limit = {};
  }

  void frame_limiter_streaming_refresh() {
    std::scoped_lock lock {g_lifecycle_mutex};
    if (g_active_provider != frame_limiter_provider::rtss || g_last_effective_limit.millihz == 0) {
      return;
    }

    const auto result =
      rtss_streaming_refresh(g_last_effective_limit.rtss_numerator, g_last_effective_limit.rtss_denominator);
    if (result == rtss_apply_result::applied) {
      BOOST_LOG(info) << "Frame limiter provider 'rtss' refreshed";
    } else if (result == rtss_apply_result::retained_exclusive) {
      BOOST_LOG(warning) << "RTSS refresh was not confirmed; keeping RTSS as the exclusive frame-limiter provider";
    } else if (rtss_hooks_stalled() && frame_limiter_nvcp::is_available()) {
      if (rtss_audit_pending_recovery() == rtss_recovery_audit_result::retained_exclusive) {
        BOOST_LOG(warning) << "RTSS recovery remains unresolved; refusing NVIDIA limiter fallback";
        return;
      }
      BOOST_LOG(warning) << "RTSS stalled while refreshing the frame limit; falling back to the NVIDIA driver";
      if (g_nvcp_started) {
        frame_limiter_nvcp::streaming_stop();
        g_nvcp_started = false;
      }
      log_nvcp_fractional_rounding(g_last_effective_limit);
      if (frame_limiter_nvcp::streaming_start(
            g_last_effective_limit.nvcp_fps,
            true,
            false,
            g_nvcp_force_vsync_off,
            false,
            g_nvcp_apply_smooth_motion
          )) {
        g_active_provider = frame_limiter_provider::nvidia_control_panel;
        g_nvcp_started = true;
        BOOST_LOG(info) << "Frame limiter provider 'nvidia-control-panel' applied after RTSS refresh failure";
      } else {
        BOOST_LOG(warning) << "NVIDIA driver frame limiter failed after RTSS refresh failure";
      }
    } else {
      BOOST_LOG(warning) << "RTSS frame-limit refresh failed without a safe fallback provider transition";
    }
  }

  frame_limiter_provider frame_limiter_active_provider() {
    std::scoped_lock lock {g_lifecycle_mutex};
    return g_active_provider;
  }

  frame_limiter_status_t frame_limiter_get_status() {
    std::scoped_lock lock {g_lifecycle_mutex};
    frame_limiter_status_t status {};
    status.enabled = config::frame_limiter.enable;
    status.configured_provider = parse_provider(config::frame_limiter.provider);
    status.active_provider = g_active_provider;
    status.nvidia_available = platf::has_nvidia_gpu();
    status.nvcp_ready = frame_limiter_nvcp::is_available();
    status.rtss_available = rtss_is_configured();
    status.disable_vsync = config::frame_limiter.disable_vsync;
    status.nv_overrides_supported = status.nvidia_available && status.nvcp_ready;
    status.rtss = rtss_get_status();
    return status;
  }

}  // namespace platf

#endif  // _WIN32

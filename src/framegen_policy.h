/**
 * @file src/framegen_policy.h
 * @brief Stream-start frame generation refresh and limiter policy helpers.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace framegen {

  struct stream_start_policy_t {
    int fps = 0;
    // Vibepollo's raw stream cadence in millihertz when it is available.
    int fps_scaled = 0;
    // Exact per-client display-mode rate. Zero means use the streamed FPS.
    std::uint32_t frame_limit_millihz = 0;
    std::string frame_generation_provider {"lossless-scaling"};
    std::optional<int> lossless_rtss_limit;
    bool smooth_motion = false;
    bool capture_fix_enabled = false;
    bool uses_virtual_display = false;
    bool requires_virtual_display = false;
    bool effective_wgc_capture = false;
    bool physical_framegen_capture = false;
    bool auto_virtual_framegen_limiter = false;
    bool frame_generation_enabled = false;
    std::optional<int> framegen_refresh_rate;
    std::optional<std::uint32_t> framegen_refresh_millihz;
    int refresh_multiplier = 1;
  };

  struct stream_start_policy_input_t {
    int fps = 0;
    int fps_scaled = 0;
    std::uint32_t display_refresh_millihz = 0;
    bool frame_generation_enabled = false;
    bool gen1_framegen_fix = false;
    bool gen2_framegen_fix = false;
    bool lossless_scaling_framegen = false;
    std::optional<int> lossless_rtss_limit;
    std::string frame_generation_provider {"lossless-scaling"};
    bool uses_virtual_display = false;
    std::string capture_mode;
    bool auto_capture_uses_wgc = false;
    bool auto_virtual_framegen_limiter = true;
    int virtual_display_refresh_multiplier = 1;
  };

  inline std::string normalize_provider(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
      if (std::isalnum(static_cast<unsigned char>(ch))) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
    }
    if (normalized == "nvidia" || normalized == "smoothmotion" || normalized == "nvidiasmoothmotion") {
      return "nvidia-smooth-motion";
    }
    if (normalized == "game" || normalized == "gameprovided" || normalized == "gameprovider") {
      return "game-provided";
    }
    if (normalized == "lossless" || normalized == "losslessscaling") {
      return "lossless-scaling";
    }
    return "lossless-scaling";
  }

  inline std::string normalize_capture_mode(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
      if (!std::isspace(static_cast<unsigned char>(ch))) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
    }
    return normalized;
  }

  inline bool provider_implies_frame_generation(std::string_view provider, bool lossless_scaling_framegen) {
    const auto normalized = normalize_provider(provider);
    if (normalized == "lossless-scaling") {
      return lossless_scaling_framegen;
    }
    return normalized == "game-provided" || normalized == "nvidia-smooth-motion";
  }

  inline int saturating_refresh_fps(int fps, int multiplier) {
    if (fps <= 0 || multiplier <= 1) {
      return fps;
    }
    if (fps > std::numeric_limits<int>::max() / multiplier) {
      return std::numeric_limits<int>::max();
    }
    return fps * multiplier;
  }

  inline std::uint32_t saturating_refresh_millihz(std::uint32_t millihz, int multiplier) {
    if (millihz == 0 || multiplier <= 1) {
      return millihz;
    }
    if (millihz > std::numeric_limits<std::uint32_t>::max() / static_cast<std::uint32_t>(multiplier)) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    return millihz * static_cast<std::uint32_t>(multiplier);
  }

  inline std::uint32_t normalize_refresh_millihz(int value) {
    if (value <= 0) {
      return 0;
    }
    const auto raw = static_cast<std::uint32_t>(value);
    return raw >= 1000 ? raw : saturating_refresh_millihz(raw, 1000);
  }

  inline int rounded_fps_from_millihz(std::uint32_t millihz) {
    constexpr std::uint32_t kMillihzPerHertz = 1000;
    const auto rounded = (static_cast<std::uint64_t>(millihz) + kMillihzPerHertz / 2) / kMillihzPerHertz;
    return rounded > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ?
             std::numeric_limits<int>::max() :
             static_cast<int>(rounded);
  }

  inline stream_start_policy_t make_stream_start_policy(const stream_start_policy_input_t &input) {
    stream_start_policy_t policy;
    policy.fps = input.fps;
    policy.fps_scaled = input.fps_scaled;
    policy.frame_limit_millihz = input.display_refresh_millihz;
    policy.frame_generation_provider = normalize_provider(input.frame_generation_provider);
    policy.lossless_rtss_limit = input.lossless_rtss_limit;
    policy.smooth_motion = policy.frame_generation_provider == "nvidia-smooth-motion";
    // Legacy capture-fix flags are kept in config for compatibility, but no longer drive
    // frame generation policy.
    policy.capture_fix_enabled = false;
    policy.uses_virtual_display = input.uses_virtual_display;
    const bool game_provided_framegen = policy.frame_generation_provider == "game-provided";

    const bool provider_enabled = provider_implies_frame_generation(
      policy.frame_generation_provider,
      input.lossless_scaling_framegen
    );
    policy.frame_generation_enabled = input.frame_generation_enabled || provider_enabled;
    policy.requires_virtual_display = false;

    const auto capture_mode = normalize_capture_mode(input.capture_mode);
    const bool hard_wgc_capture = policy.uses_virtual_display && game_provided_framegen;
    // An explicit DXGI request is a client contract.  Game-provided frame
    // generation defaults to WGC only when the capture mode is automatic.
    const bool explicit_dxgi_capture = capture_mode == "ddx";
    const bool explicit_wgc_capture = capture_mode == "wgc" || capture_mode == "wgcc";
    const bool auto_wgc_capture = capture_mode.empty() && input.auto_capture_uses_wgc;
    policy.effective_wgc_capture =
      policy.uses_virtual_display && !explicit_dxgi_capture && (explicit_wgc_capture || auto_wgc_capture || hard_wgc_capture);

    policy.physical_framegen_capture = policy.frame_generation_enabled && !policy.uses_virtual_display;

    // Virtual displays can apply a matching stream-start frame cap independently from their
    // refresh policy. The default automatic policy holds a fixed 4x display refresh; WGC
    // changes its latest-frame admission between 2x desktop and 4x game activity. Legacy
    // mode instead supplies a fixed 2x multiplier here.
    if (policy.uses_virtual_display) {
      policy.auto_virtual_framegen_limiter = input.auto_virtual_framegen_limiter;
      policy.refresh_multiplier = std::max(1, input.virtual_display_refresh_multiplier);
      if (policy.refresh_multiplier > 1 && policy.fps > 0) {
        const auto base_millihz = input.display_refresh_millihz > 0 ?
                                      input.display_refresh_millihz :
                                      (input.fps_scaled > 0 ?
                                         static_cast<std::uint32_t>(input.fps_scaled) :
                                         normalize_refresh_millihz(policy.fps));
        const auto refresh_millihz = saturating_refresh_millihz(base_millihz, policy.refresh_multiplier);
        if (refresh_millihz > 0) {
          policy.framegen_refresh_millihz = refresh_millihz;
          policy.framegen_refresh_rate = rounded_fps_from_millihz(refresh_millihz);
        }
      }
    }

    return policy;
  }

}  // namespace framegen

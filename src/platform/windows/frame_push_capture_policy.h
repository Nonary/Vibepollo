#pragma once

#include <cstdint>
#include <chrono>
#include <string_view>

namespace platf::frame_push::policy {
  inline constexpr std::string_view kCaptureMode = "idd-push";
  inline constexpr std::chrono::milliseconds kProducerStallTimeout {2000};

  constexpr bool producer_stalled(const std::chrono::milliseconds idle) noexcept {
    return idle >= kProducerStallTimeout;
  }

  enum class Selection {
    NotRequested,
    BuildDisabled,
    WrongDisplay,
    DriverUnsupported,
    HostBackendUnavailable,
    Ready,
  };

  constexpr bool requested(std::string_view capture_mode) noexcept {
    return capture_mode == kCaptureMode;
  }

  constexpr bool supports_required_capability(std::uint64_t capabilities, std::uint32_t max_ring_slots, std::uint64_t required_capabilities, std::uint32_t required_ring_slots) noexcept {
    return (capabilities & required_capabilities) == required_capabilities &&
           max_ring_slots == required_ring_slots;
  }

  constexpr bool target_matches_capture_output(
    std::uint32_t target_width, std::uint32_t target_height,
    std::uint32_t target_luid_low, std::int32_t target_luid_high,
    std::uint32_t capture_width, std::uint32_t capture_height,
    std::uint32_t capture_luid_low, std::int32_t capture_luid_high
  ) noexcept {
    return target_width != 0 && target_height != 0 &&
           (target_luid_low != 0 || target_luid_high != 0) &&
           target_width == capture_width && target_height == capture_height &&
           target_luid_low == capture_luid_low && target_luid_high == capture_luid_high;
  }

  // The first host adapter can preserve SDR P709 pixels end to end. The driver
  // protocol can describe FP16/scRGB, but the current Sunshine display path
  // does not yet carry enough color metadata to map that mode safely.
  constexpr bool supports_capture_color_mode(
    std::uint32_t format, std::uint32_t transport_color_space,
    bool output_color_space_known, std::uint32_t output_color_space
  ) noexcept {
    constexpr std::uint32_t kBgra8Unorm = 87;
    constexpr std::uint32_t kSdrP709 = 0;
    return format == kBgra8Unorm && transport_color_space == kSdrP709 &&
           output_color_space_known && output_color_space == kSdrP709;
  }

  constexpr Selection select(bool request, bool build_enabled, bool sunshine_virtual_display, bool driver_capable, bool host_backend_ready) noexcept {
    if (!request) {
      return Selection::NotRequested;
    }
    if (!build_enabled) {
      return Selection::BuildDisabled;
    }
    if (!sunshine_virtual_display) {
      return Selection::WrongDisplay;
    }
    if (!driver_capable) {
      return Selection::DriverUnsupported;
    }
    if (!host_backend_ready) {
      return Selection::HostBackendUnavailable;
    }
    return Selection::Ready;
  }

  // Every explicit request falls back until the selector has initialized a
  // live production adapter; compile support or driver capability alone is
  // insufficient.
  constexpr bool use_wgc_fallback(Selection selection) noexcept {
    return selection != Selection::NotRequested && selection != Selection::Ready;
  }

  constexpr std::string_view fallback_reason(Selection selection) noexcept {
    switch (selection) {
      case Selection::NotRequested:
        return "not requested";
      case Selection::BuildDisabled:
        return "frame-push support is disabled in this build";
      case Selection::WrongDisplay:
        return "the selected output is not a Sunshine virtual display";
      case Selection::DriverUnsupported:
        return "the installed driver does not advertise the required frame ring";
      case Selection::HostBackendUnavailable:
        return "the production frame-push capture session could not be initialized";
      case Selection::Ready:
        return "ready";
    }
    return "unknown frame-push selection state";
  }
}  // namespace platf::frame_push::policy

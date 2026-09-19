#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace platf::dxgi::wgc_policy {
  inline constexpr std::uint32_t low_latency_initial_buffer_size = 1;
  inline constexpr std::uint32_t adaptive_max_buffer_size = 2;
  inline constexpr std::uint32_t helper_stop_timeout_ms = 3000;

  /**
   * Bound WGC helper publication without turning source jitter into frame loss.
   *
   * A minimum-time-since-last-frame gate loses any lateness accumulated by the
   * previous callback. An early callback following that late one is then
   * rejected even when the pair is at or below the configured average rate.
   * Keep one frame of phase credit so uneven compositor callbacks can repay one
   * another, while a two-frame credit cap still coalesces sustained oversupply.
   */
  class activity_frame_limiter_t {
  public:
    using clock_t = std::chrono::steady_clock;

    void reset(const int frames_per_second) noexcept {
      interval_ = frames_per_second > 0 ?
                    std::chrono::nanoseconds(std::chrono::seconds(1)) / frames_per_second :
                    std::chrono::nanoseconds::zero();
      credit_ = clock_t::duration::zero();
      last_admission_.reset();
    }

    [[nodiscard]] bool admit(const clock_t::time_point arrival) noexcept {
      if (interval_ <= clock_t::duration::zero()) {
        return true;
      }

      if (!last_admission_) {
        last_admission_ = arrival;
        // Preserve one frame of source phase so a short/long jitter pair can
        // begin in either order without losing the short half.
        credit_ = interval_;
        return true;
      }

      const auto elapsed = arrival > *last_admission_ ?
                             arrival - *last_admission_ :
                             clock_t::duration::zero();
      const auto available_credit = (std::min)(credit_capacity(), credit_ + elapsed);
      if (available_credit < interval_) {
        return false;
      }

      // A long stall admits its resumed frame immediately, but must not bank a
      // burst of catch-up deliveries. Short jitter retains only unused credit.
      credit_ = elapsed >= credit_capacity() ?
                  clock_t::duration::zero() :
                  available_credit - interval_;
      last_admission_ = arrival;
      return true;
    }

  private:
    [[nodiscard]] clock_t::duration credit_capacity() const noexcept {
      return interval_ + interval_;
    }

    clock_t::duration interval_ {};
    clock_t::duration credit_ {};
    std::optional<clock_t::time_point> last_admission_;
  };

  // Absolute input uses the whole virtual desktop, not just the captured
  // monitor. A neighbouring monitor can change these values without moving
  // or resizing the capture target itself.
  struct input_geometry_t {
    int offset_x;
    int offset_y;
    int desktop_width;
    int desktop_height;

    constexpr bool operator==(const input_geometry_t &) const = default;
  };

  struct desktop_bounds_t {
    int origin_x;
    int origin_y;
    int width;
    int height;
  };

  enum class input_geometry_change_e {
    unchanged,
    changed,
    unavailable,
  };

  constexpr input_geometry_change_e assess_input_geometry(
    const input_geometry_t &captured,
    const int monitor_x,
    const int monitor_y,
    const desktop_bounds_t &current
  ) noexcept {
    // GetSystemMetrics returns zero on failure. Preserve capture while the
    // desktop is temporarily unavailable, as with other DXGI settle retries.
    if (current.width <= 0 || current.height <= 0) {
      return input_geometry_change_e::unavailable;
    }
    const bool unchanged =
      captured.offset_x == static_cast<std::int64_t>(monitor_x) - current.origin_x &&
      captured.offset_y == static_cast<std::int64_t>(monitor_y) - current.origin_y &&
      captured.desktop_width == current.width && captured.desktop_height == current.height;
    return unchanged ? input_geometry_change_e::unchanged : input_geometry_change_e::changed;
  }

  /**
   * Select only the requested monitor, allowing transient enumeration failures
   * to settle. A missing explicit target must never turn into primary capture.
   * The caller supplies the bounded wait and the platform monitor lookups.
   */
  template<class FindRequested, class FindPrimary, class WaitForRetry>
  auto select_monitor(
    const bool has_requested_monitor,
    FindRequested find_requested,
    FindPrimary find_primary,
    WaitForRetry wait_for_retry
  ) {
    if (!has_requested_monitor) {
      return find_primary();
    }

    auto monitor = find_requested();
    while (!monitor && wait_for_retry()) {
      monitor = find_requested();
    }
    return monitor;
  }

  enum class capture_surface_format : std::uint8_t {
    bgra8,
    rgba16_float,
  };

  constexpr capture_surface_format select_capture_surface_format(
    const bool config_received,
    const bool force_sdr_capture,
    const bool dynamic_range,
    const bool advanced_color_capture
  ) noexcept {
    return config_received &&
             !force_sdr_capture &&
             (dynamic_range || advanced_color_capture) ?
             capture_surface_format::rgba16_float :
             capture_surface_format::bgra8;
  }

  constexpr std::uint32_t maximum_buffer_size(const bool vrr_low_latency) noexcept {
    return vrr_low_latency ? low_latency_initial_buffer_size : adaptive_max_buffer_size;
  }

  constexpr bool buffer_pool_is_quiet(
    const bool allow_decrease,
    const bool has_recent_drop,
    const bool recent_pool_pressure,
    const int peak_outstanding,
    const std::uint32_t current_buffer_size
  ) noexcept {
    return allow_decrease &&
           !has_recent_drop &&
           !recent_pool_pressure &&
           peak_outstanding <= static_cast<int>(current_buffer_size) - 1;
  }
}  // namespace platf::dxgi::wgc_policy

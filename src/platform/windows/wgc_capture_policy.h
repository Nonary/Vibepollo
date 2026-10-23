#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace platf::dxgi::wgc_policy {
  inline constexpr std::uint32_t low_latency_initial_buffer_size = 1;
  inline constexpr std::uint32_t adaptive_max_buffer_size = 2;
  inline constexpr std::uint32_t helper_stop_timeout_ms = 3000;

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

  // Portable identity for the DXGI output being captured. Input-only geometry
  // refresh is allowed only when this full target identity still matches.
  struct output_target_identity_t {
    std::int32_t adapter_high_part;
    std::uint32_t adapter_low_part;
    std::wstring_view output_name;
    std::uintptr_t monitor;
    bool attached_to_desktop;
    int rect_left;
    int rect_top;
    int rect_right;
    int rect_bottom;
    std::uint32_t rotation;
    bool hdr_valid;
    bool hdr;
  };

  constexpr bool matches_captured_target(
    const output_target_identity_t &captured,
    const output_target_identity_t &observed
  ) noexcept {
    return captured.adapter_high_part == observed.adapter_high_part &&
           captured.adapter_low_part == observed.adapter_low_part &&
           captured.output_name == observed.output_name &&
           captured.attached_to_desktop == observed.attached_to_desktop &&
           observed.attached_to_desktop &&
           captured.monitor == observed.monitor &&
           captured.rect_left == observed.rect_left &&
           captured.rect_top == observed.rect_top &&
           captured.rect_right == observed.rect_right &&
           captured.rect_bottom == observed.rect_bottom &&
           captured.rotation == observed.rotation &&
           captured.hdr_valid == observed.hdr_valid &&
           captured.hdr == observed.hdr;
  }

  enum class input_geometry_change_e {
    unchanged,
    changed,
    unavailable,
  };

  constexpr bool input_geometry_change_requires_capture_restart(
    const bool preserve_capture,
    const input_geometry_change_e change
  ) noexcept {
    return !preserve_capture && change == input_geometry_change_e::changed;
  }

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

  constexpr bool input_geometry_observation_needed(
    const bool transport_active,
    const bool mapping_valid,
    const input_geometry_change_e change
  ) noexcept {
    return transport_active && (!mapping_valid || change != input_geometry_change_e::unchanged);
  }

  constexpr bool target_observation_needed(
    const bool notified,
    const bool factory_stale,
    const bool geometry_observation_needed,
    const bool color_events_subscribed,
    const bool retry_pending,
    const bool verify_target_periodically,
    const bool poll_due
  ) noexcept {
    return notified || factory_stale || geometry_observation_needed ||
           ((!color_events_subscribed || retry_pending || verify_target_periodically) && poll_due);
  }

  enum class verified_target_state_e : std::uint8_t {
    same,
    changed,
    unavailable,
  };

  enum class active_capture_state_e : std::uint8_t {
    valid,
    invalid,
    item_closed,
  };

  enum class geometry_update_action_e : std::uint8_t {
    unchanged,
    publish,
    invalidate_and_retry,
    retry,
    reinitialize,
  };

  template<class Invalidate>
  bool invalidate_unverified_target_geometry_before_retry(
    const bool input_geometry_transport_active,
    bool &mapping_valid,
    Invalidate &&invalidate
  ) {
    if (!input_geometry_transport_active) return false;
    if (mapping_valid) std::forward<Invalidate>(invalidate)();
    mapping_valid = false;
    return true;
  }

  template<class Invalidate>
  bool invalidate_changed_target_geometry_before_reinitialize(
    const bool input_geometry_transport_active,
    bool &mapping_valid,
    Invalidate &&invalidate
  ) {
    if (!input_geometry_transport_active) return false;
    if (mapping_valid) std::forward<Invalidate>(invalidate)();
    mapping_valid = false;
    return true;
  }

  struct geometry_update_decision_t {
    geometry_update_action_e action;
    input_geometry_t mapping;
  };

  constexpr geometry_update_decision_t assess_verified_geometry_update(
    const active_capture_state_e capture_state,
    const verified_target_state_e target_state,
    const bool mapping_valid,
    const input_geometry_t &captured,
    const int target_x,
    const int target_y,
    const desktop_bounds_t &current
  ) noexcept {
    if (capture_state != active_capture_state_e::valid ||
        target_state == verified_target_state_e::changed) {
      return {geometry_update_action_e::reinitialize, captured};
    }
    if (target_state == verified_target_state_e::unavailable ||
        current.width <= 0 || current.height <= 0) {
      return {
        mapping_valid ? geometry_update_action_e::invalidate_and_retry : geometry_update_action_e::retry,
        captured,
      };
    }

    const auto offset_x = static_cast<std::int64_t>(target_x) - current.origin_x;
    const auto offset_y = static_cast<std::int64_t>(target_y) - current.origin_y;
    if (offset_x < std::numeric_limits<int>::min() ||
        offset_x > std::numeric_limits<int>::max() ||
        offset_y < std::numeric_limits<int>::min() ||
        offset_y > std::numeric_limits<int>::max()) {
      return {
        mapping_valid ? geometry_update_action_e::invalidate_and_retry : geometry_update_action_e::retry,
        captured,
      };
    }

    const input_geometry_t updated {
      static_cast<int>(offset_x),
      static_cast<int>(offset_y),
      current.width,
      current.height,
    };
    return {
      updated == captured && mapping_valid ? geometry_update_action_e::unchanged : geometry_update_action_e::publish,
      updated,
    };
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

  /**
   * Direct3D11CaptureFrame::SystemRelativeTime is a TimeSpan: 100 ns units on
   * the QPC timebase, not raw QPC ticks. The two only coincide when the
   * performance counter runs at 10 MHz.
   */
  constexpr std::uint64_t system_relative_time_to_qpc(const std::int64_t hundred_ns, const std::int64_t frequency) noexcept {
    constexpr std::int64_t units_per_second = 10'000'000;
    if (hundred_ns <= 0) {
      return 0;
    }
    if (frequency <= 0 || frequency == units_per_second) {
      return static_cast<std::uint64_t>(hundred_ns);
    }
    return static_cast<std::uint64_t>(
      (hundred_ns / units_per_second) * frequency +
      (hundred_ns % units_per_second) * frequency / units_per_second
    );
  }

  // This is a minimum-gap source ceiling, with no callback credits or catch-up
  // budget. FrameArrived scheduling jitter must not lower the compositor rate.
  class source_admission_t {
  public:
    bool admit(const int fps, const std::int64_t source_100ns, const std::chrono::nanoseconds callback_time) noexcept {
      if (fps <= 0) {
        *this = {};
        return true;
      }

      const bool advancing = source_100ns > 0 && source_100ns > _last_observed_source;
      if (source_100ns > 0) {
        _last_observed_source = source_100ns;
      }
      if (!advancing) {
        // Missing/repeated/backwards timestamps cannot supply source cadence.
        // Keep changing content live with bounded arrival fallback until a fresh
        // source anchor is admitted.
        _source_anchored = false;
      }

      const auto source_gap = 10'000'000LL / fps;
      const auto callback_gap = 1'000'000'000LL / fps;
      if (_has_admitted) {
        if (advancing && _source_anchored) {
          // Floor in the timestamp's native 100 ns units, so a quantized exact
          // 60 FPS source (166666/166667 ticks) still meets a 60 FPS ceiling.
          if (source_100ns - _last_admitted_source < (source_gap > 0 ? source_gap : 1)) {
            return false;
          }
        } else if ((callback_time - _last_admitted_callback).count() < (callback_gap > 0 ? callback_gap : 1)) {
          return false;
        }
      }

      _has_admitted = true;
      _last_admitted_callback = callback_time;
      _source_anchored = advancing;
      if (advancing) {
        _last_admitted_source = source_100ns;
      }
      return true;
    }

  private:
    bool _has_admitted = false;
    bool _source_anchored = false;
    std::int64_t _last_observed_source = 0;
    std::int64_t _last_admitted_source = 0;
    std::chrono::nanoseconds _last_admitted_callback {};
  };

  constexpr std::uint32_t maximum_buffer_size(const bool vrr_low_latency) noexcept {
    return vrr_low_latency ? low_latency_initial_buffer_size : adaptive_max_buffer_size;
  }

  /**
   * A new WGC frame may skip the scratch handoff and go straight into the
   * shared texture only when no older frame is queued or mid-delivery;
   * otherwise it would be published ahead of that older frame.
   */
  constexpr bool may_publish_directly(const bool stopping, const bool frame_pending, const bool frame_delivering) noexcept {
    return !stopping && !frame_pending && !frame_delivering;
  }

  /**
   * How far an encoder reading the helper's shared texture directly has got:
   * the frame-record generation and the newest frame id it converted.
   */
  struct direct_frame_cursor_t {
    std::uint64_t generation = 0;
    std::uint64_t frame_id = 0;
  };

  /**
   * Advance the cursor to the frame now in the shared texture. Returns false
   * when the encoder already converted that frame, so encoding it again would
   * repeat it. A new record generation (a restarted helper) restarts frame ids.
   */
  constexpr bool take_direct_frame(direct_frame_cursor_t &cursor, const std::uint64_t generation, const std::uint64_t frame_id) noexcept {
    if (cursor.generation != generation) {
      cursor = {generation, frame_id};
      return true;
    }
    if (frame_id <= cursor.frame_id) {
      return false;
    }
    cursor.frame_id = frame_id;
    return true;
  }

  enum class direct_frame_e : std::uint8_t {
    convert,  ///< A frame this encoder has not converted yet.
    repeat,  ///< Pixels this encoder already converted.
    unreadable,  ///< The frame record could not be read, so the frame is unidentified.
  };

  /**
   * Decide what an encoder does with the frame now in the shared texture.
   * An unreadable frame record skips the frame and leaves the cursor alone:
   * the encoder cannot tell which frame it holds, so it must not encode it or
   * advance past frames it never converted. (A record is unreadable here only
   * if the helper died mid-publish, which the IPC session turns into a capture
   * restart.)
   */
  constexpr direct_frame_e classify_direct_frame(direct_frame_cursor_t &cursor, const bool record_readable, const std::uint64_t generation, const std::uint64_t frame_id) noexcept {
    if (!record_readable) {
      return direct_frame_e::unreadable;
    }
    return take_direct_frame(cursor, generation, frame_id) ? direct_frame_e::convert : direct_frame_e::repeat;
  }

  /**
   * Whether GPU encoders may read the helper's shared texture directly. The
   * copy path stays for sources that need a stable image of their own: constant
   * frame rate capture re-sends its cached image on a timeout, which the
   * frame cursor would drop as a repeat, and PyroWave pins and reads the
   * capture image's own texture and keyed mutex.
   */
  constexpr bool direct_encoder_input_allowed(const bool enabled, const bool constant_frame_rate, const bool pyrowave) noexcept {
    return enabled && !constant_frame_rate && !pyrowave;
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

/**
 * @file src/platform/linux/kmsgrab_pacing.h
 * @brief Pure pacing helpers for event-driven KMS capture.
 */
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace platf::kms::pacing {

  using clock_t = std::chrono::steady_clock;
  constexpr auto PRESENT_PENDING_HANG_TIMEOUT = std::chrono::seconds {5};

  enum class presentation_response_e {
    changed,
    timeout,
    invalid,
  };

  enum class presentation_ioctl_error_e {
    retry,
    transient_timeout,
    unsupported,
  };

  class presentation_mode_t {
  public:
    explicit presentation_mode_t(bool required = false):
        required_ {required} {
    }

    void activate() {
      enabled_ = true;
    }

    void deactivate() {
      enabled_ = false;
    }

    [[nodiscard]] bool event_capture_enabled() const {
      return enabled_;
    }

    [[nodiscard]] bool fixed_rate_allowed() const {
      return !required_;
    }

  private:
    bool required_ {false};
    bool enabled_ {false};
  };

  class presentation_rate_limiter_t {
  public:
    void set_interval(clock_t::duration interval) {
      interval_ = interval;
      next_delivery_.reset();
    }

    void reset() {
      next_delivery_.reset();
    }

    [[nodiscard]] clock_t::time_point next_delivery(clock_t::time_point now) const {
      if (!next_delivery_ || interval_ <= clock_t::duration::zero()) {
        return now;
      }
      return *next_delivery_;
    }

    void mark_delivered(clock_t::time_point delivery) {
      if (interval_ <= clock_t::duration::zero()) {
        next_delivery_.reset();
        return;
      }

      if (!next_delivery_) {
        next_delivery_ = delivery + interval_;
        return;
      }

      /*
       * Follow the compositor clock when its presentation is close to the
       * negotiated cadence. This keeps equal-rate generated and real frames
       * in their original slots instead of resampling them onto a competing
       * capture clock. A materially early presentation is genuine oversupply,
       * so retain the negotiated schedule and coalesce excess frames there.
       * Late presentations rebase immediately and never produce catch-up
       * bursts.
       */
      const auto tracking_tolerance = interval_ / 8;
      if (delivery + tracking_tolerance >= *next_delivery_) {
        next_delivery_ = delivery + interval_;
      } else {
        next_delivery_ = *next_delivery_ + interval_;
      }
    }

  private:
    clock_t::duration interval_ {};
    std::optional<clock_t::time_point> next_delivery_;
  };

  /**
   * Smooth packet presentation timestamps toward the negotiated client cadence
   * without letting that cadence drift away from the source clock. Virtual KMS
   * timestamps contain small VRR submission jitter, but a free-running exact
   * grid can outrun a client's real (fractional) refresh rate and grow its frame
   * queue. This acts as a lightweight phase-locked loop: remove most short-term
   * jitter while continuously correcting long-term phase and frequency error.
   */
  class presentation_timestamp_grid_t {
  public:
    void set_interval(clock_t::duration interval) {
      interval_ = interval;
      reset();
    }

    void reset() {
      next_timestamp_.reset();
    }

    [[nodiscard]] clock_t::time_point normalize(clock_t::time_point source_timestamp) {
      if (interval_ <= clock_t::duration::zero()) {
        return source_timestamp;
      }

      if (!next_timestamp_) {
        next_timestamp_ = source_timestamp + interval_;
        return source_timestamp;
      }

      const auto scheduled = *next_timestamp_;
      const auto phase_error = source_timestamp - scheduled;

      /*
       * A phase error beyond half a frame is a real missed slot or discontinuity,
       * not normal VRR jitter. Preserve it immediately. Otherwise correct one
       * quarter of the phase error per frame, which damps alternating jitter but
       * converges to the source frequency instead of accumulating queue depth.
       */
      if (phase_error > interval_ / 2 || phase_error < -interval_ / 2) {
        next_timestamp_ = source_timestamp + interval_;
        return source_timestamp;
      }

      const auto normalized = scheduled + phase_error / 4;
      next_timestamp_ = normalized + interval_;
      return normalized;
    }

  private:
    clock_t::duration interval_ {};
    std::optional<clock_t::time_point> next_timestamp_;
  };

  class presentation_latch_t {
  public:
    using generation_t = std::uint64_t;

    void request_capture() {
      capture_needed_ = true;
      ++generation_;
    }

    void observe_response(presentation_response_e response, bool pending, clock_t::time_point now) {
      if (response == presentation_response_e::changed) {
        request_capture();
      }

      state_pending_ = pending;
      if (pending) {
        if (!pending_since_) {
          pending_since_ = now;
        }
      } else {
        pending_since_.reset();
      }
    }

    [[nodiscard]] bool capture_ready() const {
      return capture_needed_ && !state_pending_;
    }

    [[nodiscard]] bool state_pending() const {
      return state_pending_;
    }

    [[nodiscard]] bool pending_timed_out(clock_t::time_point now, clock_t::duration limit) const {
      return pending_since_ && now - *pending_since_ >= limit;
    }

    [[nodiscard]] generation_t capture_generation() const {
      return generation_;
    }

    void mark_delivered(generation_t delivered_generation) {
      if (delivered_generation == generation_) {
        capture_needed_ = false;
      }
    }

  private:
    bool capture_needed_ {false};
    bool state_pending_ {false};
    generation_t generation_ {};
    std::optional<clock_t::time_point> pending_since_;
  };

  inline presentation_ioctl_error_e classify_ioctl_error(int error, bool blocking_wait) {
    if (blocking_wait && (error == EINTR || error == EAGAIN)) {
      return presentation_ioctl_error_e::retry;
    }
    if (blocking_wait && error == EBUSY) {
      return presentation_ioctl_error_e::transient_timeout;
    }
    return presentation_ioctl_error_e::unsupported;
  }

  inline bool hold_pending_response_to_deadline(
    presentation_response_e response,
    bool pending,
    bool blocking_wait
  ) {
    return response == presentation_response_e::changed && pending && blocking_wait;
  }

  inline presentation_response_e classify_response(
    std::uint32_t flags,
    std::uint32_t changed_flag,
    std::uint32_t timeout_flag,
    std::uint32_t pending_flag,
    std::uint64_t requested_sequence,
    std::uint64_t returned_sequence
  ) {
    const auto known_flags = changed_flag | timeout_flag | pending_flag;
    if ((flags & ~known_flags) != 0) {
      return presentation_response_e::invalid;
    }

    const auto result_flag = flags & ~pending_flag;
    if (result_flag == changed_flag && returned_sequence > requested_sequence) {
      return presentation_response_e::changed;
    }

    if (result_flag == timeout_flag && returned_sequence == requested_sequence) {
      return presentation_response_e::timeout;
    }

    return presentation_response_e::invalid;
  }

  inline std::optional<clock_t::time_point> validate_timestamp(
    std::uint64_t timestamp_ns,
    clock_t::time_point now,
    clock_t::duration future_tolerance,
    std::optional<clock_t::time_point> minimum_timestamp = std::nullopt
  ) {
    using rep_t = std::chrono::nanoseconds::rep;
    if (timestamp_ns == 0 || timestamp_ns > static_cast<std::uint64_t>(std::numeric_limits<rep_t>::max())) {
      return std::nullopt;
    }

    const auto timestamp = clock_t::time_point {
      std::chrono::nanoseconds {static_cast<rep_t>(timestamp_ns)}
    };
    if (timestamp > now + future_tolerance || (minimum_timestamp && timestamp < *minimum_timestamp)) {
      return std::nullopt;
    }

    return timestamp;
  }

}  // namespace platf::kms::pacing

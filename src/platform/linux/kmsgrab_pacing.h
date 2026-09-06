/**
 * @file src/platform/linux/kmsgrab_pacing.h
 * @brief Pure pacing helpers for event-driven KMS capture.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace platf::kms::pacing {

  using clock_t = std::chrono::steady_clock;
  constexpr auto PRESENT_PENDING_HANG_TIMEOUT = std::chrono::seconds {5};

  [[nodiscard]] constexpr std::chrono::nanoseconds interval_from_frame_rate(
    std::int64_t numerator,
    std::int64_t denominator
  ) {
    if (numerator <= 0 || denominator <= 0) {
      return std::chrono::nanoseconds::zero();
    }

    return std::chrono::nanoseconds {
      denominator * std::chrono::nanoseconds::period::den / numerator
    };
  }

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
    struct diagnostic_state_t {
      clock_t::duration interval {};
      clock_t::duration stored_credit {};
      clock_t::duration available_credit {};
      std::optional<clock_t::time_point> last_delivery;
      clock_t::time_point next_delivery {};
    };

    void set_interval(clock_t::duration interval) {
      interval_ = interval;
      reset();
    }

    void reset() {
      credit_ = clock_t::duration::zero();
      last_delivery_.reset();
    }

    [[nodiscard]] clock_t::time_point next_delivery(clock_t::time_point now) const {
      if (!last_delivery_ || interval_ <= clock_t::duration::zero()) {
        return now;
      }

      const auto available_credit = replenished_credit(now);
      if (available_credit >= interval_) {
        return now;
      }
      return now + (interval_ - available_credit);
    }

    [[nodiscard]] diagnostic_state_t diagnostic_state(clock_t::time_point now) const {
      diagnostic_state_t state {
        .interval = interval_,
        .stored_credit = credit_,
        .last_delivery = last_delivery_,
        .next_delivery = now,
      };
      if (!last_delivery_ || interval_ <= clock_t::duration::zero()) {
        return state;
      }

      state.available_credit = replenished_credit(now);
      if (state.available_credit < interval_) {
        state.next_delivery = now + (interval_ - state.available_credit);
      }
      return state;
    }

    void mark_delivered(clock_t::time_point delivery) {
      if (interval_ <= clock_t::duration::zero()) {
        reset();
        return;
      }

      if (!last_delivery_) {
        last_delivery_ = delivery;

        // Start with one frame of phase credit. This lets an uneven but
        // correctly averaged VRR source lead with either the short or long
        // half of a presentation pair without adding a competing phase.
        credit_ = interval_;
        return;
      }

      /*
       * Keep at most one frame of saved phase in addition to the frame being
       * admitted. A long presentation interval can therefore pay for a short
       * one, preserving uneven VRR timing when the pair averages to the client
       * rate. Persistent oversupply consumes the finite credit and is then
       * coalesced at the negotiated ceiling.
       *
       * A gap of two frame intervals is a stall, not useful phase credit. The
       * resumed frame is immediate, but it cannot trigger a catch-up burst.
       */
      const auto elapsed = delivery > *last_delivery_ ? delivery - *last_delivery_ : clock_t::duration::zero();
      if (elapsed >= credit_capacity()) {
        credit_ = clock_t::duration::zero();
        last_delivery_ = delivery;
        return;
      }

      const auto available_credit = std::min(credit_capacity(), credit_ + elapsed);
      credit_ = available_credit > interval_ ? available_credit - interval_ : clock_t::duration::zero();
      last_delivery_ = delivery;
    }

  private:
    [[nodiscard]] clock_t::duration credit_capacity() const {
      return interval_ + interval_;
    }

    [[nodiscard]] clock_t::duration replenished_credit(clock_t::time_point now) const {
      const auto elapsed = now > *last_delivery_ ? now - *last_delivery_ : clock_t::duration::zero();
      return std::min(credit_capacity(), credit_ + elapsed);
    }

    clock_t::duration interval_ {};
    clock_t::duration credit_ {};
    std::optional<clock_t::time_point> last_delivery_;
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
        // A newly completed sequence is forward progress. Start a fresh hang
        // deadline even when another submitted commit is already pending.
        if (response == presentation_response_e::changed || !pending_since_) {
          pending_since_ = now;
        }
      } else {
        pending_since_.reset();
      }
    }

    [[nodiscard]] bool capture_ready() const {
      // PENDING describes the next submitted commit. The latest completed
      // framebuffer remains coherent and should be consumed immediately.
      return capture_needed_;
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

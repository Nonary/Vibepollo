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

  inline bool capture_due(bool presentation_pending, clock_t::time_point now, clock_t::time_point next_capture) {
    return presentation_pending && now >= next_capture;
  }

  inline clock_t::time_point capture_deadline(clock_t::time_point capture_started, clock_t::duration interval) {
    return capture_started + interval;
  }

  inline clock_t::time_point coalescing_wake_deadline(
    clock_t::time_point now,
    clock_t::time_point next_capture,
    clock_t::duration control_interval
  ) {
    return std::min(next_capture, now + control_interval);
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

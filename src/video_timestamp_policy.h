/**
 * @file video_timestamp_policy.h
 * @brief Preserve presentation timing while retaining legacy timestamp normalization.
 */
#pragma once

#include <chrono>
#include <optional>

namespace video {
  enum class source_timestamp_policy_e {
    normalize,
    preserve,
  };

  /** One encoder/display generation owns one immutable timestamp policy. */
  class encode_timestamp_policy_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using timestamp_t = std::optional<clock_t::time_point>;

    encode_timestamp_policy_t(source_timestamp_policy_e policy, clock_t::duration interval):
        policy_ {policy},
        interval_ {interval} {
    }

    [[nodiscard]] timestamp_t apply(timestamp_t source) {
      // Missing source time denotes synthetic input. It must not seed or
      // advance the real-frame timeline. Presentation sources already carry
      // their cadence; snapping them would change a timestamp-aware client's
      // intended display spacing, even below the negotiated stream ceiling.
      if (!source || policy_ == source_timestamp_policy_e::preserve) {
        return source;
      }

      if (!next_timestamp_) {
        next_timestamp_ = source;
      }
      const auto difference = *source > *next_timestamp_ ?
                                *source - *next_timestamp_ :
                                *next_timestamp_ - *source;
      if (difference < interval_ / 4) {
        source = next_timestamp_;
      } else {
        next_timestamp_ = source;
      }
      *next_timestamp_ += interval_;
      return source;
    }

  private:
    const source_timestamp_policy_e policy_;
    const clock_t::duration interval_;
    timestamp_t next_timestamp_;
  };
}  // namespace video

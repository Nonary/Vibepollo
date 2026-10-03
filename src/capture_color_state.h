#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace video::capture_color {
  enum class action_e { retain, hold, reinit, error };

  // Source color observation policy. State belongs to one immutable capture
  // generation; it never changes the encoder's interpretation of that generation.
  class transition_t {
  public:
    using clock = std::chrono::steady_clock;
    explicit transition_t(bool hdr): hdr_(hdr) {}

    void notified(clock::time_point now, std::optional<bool> expected = {}) {
      expected_ = expected;
      validation_deadline_ = now + std::chrono::seconds(2);
    }

    bool validation_pending() const { return validation_deadline_.has_value(); }

    action_e mutation(std::uint64_t revision, bool active, bool failed) {
      if (failed) return action_e::error;
      if (active) { mutation_revision_ = revision; return action_e::hold; }
      if (mutation_revision_ && *mutation_revision_ != revision) return action_e::reinit;
      mutation_revision_ = revision;
      return action_e::retain;
    }

    action_e observed(std::optional<bool> hdr, clock::time_point now) {
      if (!hdr) {
        if (!validation_deadline_) notified(now);
        return now >= *validation_deadline_ ? action_e::reinit : action_e::hold;
      }
      if (*hdr != hdr_) {
        if (!candidate_ || *candidate_ != *hdr) { candidate_ = hdr; candidate_since_ = now; }
        if (!validation_deadline_) notified(now);
        // Read twice across the existing display-helper confirmation interval.
        // This is a coalescing policy, not a claim that Windows settles in 250ms.
        return now - candidate_since_ >= std::chrono::milliseconds(250) ? action_e::reinit : action_e::hold;
      }
      candidate_.reset();
      if (expected_ && *expected_ != *hdr) {
        return validation_deadline_ && now >= *validation_deadline_ ? action_e::reinit : action_e::hold;
      }
      expected_.reset();
      validation_deadline_.reset();
      return action_e::retain;
    }

  private:
    const bool hdr_;
    std::optional<bool> candidate_, expected_;
    clock::time_point candidate_since_ {};
    std::optional<clock::time_point> validation_deadline_;
    std::optional<std::uint64_t> mutation_revision_;
  };
}

#pragma once

#include <chrono>

namespace VDISPLAY::policy {
  class inactive_reactivation_budget_t {
  public:
    using duration = std::chrono::steady_clock::duration;

    static constexpr auto attempt_interval = std::chrono::milliseconds(750);
    static constexpr auto fallback_grace = std::chrono::seconds(12);
    static constexpr unsigned int max_attempts = 3;

    bool consume_if_due(duration inactive_for) noexcept {
      const auto next_attempt_age = attempt_interval * static_cast<int>(attempts_ + 1);
      if (attempts_ >= max_attempts || inactive_for < next_attempt_age) {
        return false;
      }
      ++attempts_;
      return true;
    }

    void reset() noexcept {
      attempts_ = 0;
    }

    unsigned int attempts() const noexcept {
      return attempts_;
    }

  private:
    unsigned int attempts_ {0};
  };
}  // namespace VDISPLAY::policy

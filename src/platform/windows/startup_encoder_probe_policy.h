#pragma once

namespace video::startup_encoder_probe_policy {
  struct state {
    bool cache_successful = false;
    bool stream_active = false;
    bool shutting_down = false;
    unsigned int attempts = 0;
    unsigned int max_attempts = 0;
  };

  // A failed attempt is retryable until the explicit startup budget is
  // exhausted. Merely recording that a probe was attempted is never a
  // completion condition.
  constexpr bool should_probe(const state &value) noexcept {
    return !value.cache_successful &&
           !value.stream_active &&
           !value.shutting_down &&
           value.attempts < value.max_attempts;
  }

  constexpr bool terminal_failure(const state &value) noexcept {
    return !value.cache_successful &&
           !value.stream_active &&
           !value.shutting_down &&
           value.max_attempts != 0 &&
           value.attempts >= value.max_attempts;
  }
}  // namespace video::startup_encoder_probe_policy

#pragma once

namespace platf::startup_display_policy {
  struct state {
    bool interactive_desktop = false;
    bool stream_active = false;
    bool shutting_down = false;
  };

  // Display-driver recovery and encoder probing may block while Windows is
  // still switching from the secure/login desktop. Keep that work out of the
  // pre-listener path, and let the first session own the display if it arrives
  // before startup convergence completes.
  constexpr bool should_run(const state &value) noexcept {
    return value.interactive_desktop && !value.stream_active && !value.shutting_down;
  }

  constexpr bool should_retry(const state &value) noexcept {
    return !value.interactive_desktop && !value.stream_active && !value.shutting_down;
  }
}  // namespace platf::startup_display_policy

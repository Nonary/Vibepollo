#pragma once

#include <chrono>
#include <optional>

namespace playnite_launcher::focus::policy {

  using clock = std::chrono::steady_clock;

  // Playnite reports gameStarted when the game process spawns, which can be
  // long before the game shows a window (launchers, shader compilation, intro
  // splashes). Autofocus keeps waiting this long for a game window to appear.
  inline constexpr std::chrono::seconds k_window_wait_limit {180};

  // Returns when autofocus should stop. `armed_at` is the latest Playnite status
  // that (re)requested focus; `last_new_window_at` is when a game window not
  // seen before last appeared. The focus timeout runs from whichever is later,
  // so a game whose window shows up late still gets the full focus window.
  [[nodiscard]] clock::time_point focus_deadline(
    clock::time_point armed_at,
    std::optional<clock::time_point> last_new_window_at,
    std::chrono::seconds focus_timeout
  );

}  // namespace playnite_launcher::focus::policy

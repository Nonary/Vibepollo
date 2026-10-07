/**
 * @file src/platform/windows/game_activity.h
 * @brief Shared prioritized game-activity signals for virtual-display policy consumers.
 */
#pragma once

#include "foreground_app.h"
#include "game_activity_policy.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <winsock2.h>
#include <windows.h>

namespace platf::game_activity {

  using signal_source_e = game_activity_policy::signal_source_e;
  using signal_t = game_activity_policy::signal_t;
  using state_t = game_activity_policy::state_t;

  state_t reduce_signals(std::span<const signal_t> signals);
  const char *source_name(signal_source_e source);

  /**
   * @brief Whether a virtual-display mode set is in flight or still settling.
   *
   * Anything that calls into D3D11 or the display stack can block for the duration
   * of a mode change, so work that can be deferred (creating GPU features, tearing
   * them down) should stay out of this window.
   */
  bool display_mode_change_in_flight();
  bool preserve_confirmed_game_during_display_transition(
    const foreground_app::state_t &sample,
    const foreground_app::state_t &last_confirmed,
    bool transition_settling,
    bool minimum_hold_active = false
  );

  struct refresh_target_options_t {
    std::string display_name;
    std::string device_id;
    RECT capture_rect {};
    std::uint32_t base_refresh_numerator {0};
    std::uint32_t base_refresh_denominator {1};
    std::uint32_t high_refresh_numerator {0};
    std::uint32_t high_refresh_denominator {1};
    bool initial_high {false};
    // When supplied, update capture admission instead of changing the display mode.
    // It must not touch display-mode or capture-device state; it runs on the
    // activity worker.
    std::function<bool(bool)> apply_activity_state;
  };

  class refresh_target_t {
  public:
    ~refresh_target_t();

    refresh_target_t(const refresh_target_t &) = delete;
    refresh_target_t &operator=(const refresh_target_t &) = delete;

  private:
    struct impl_t;
    explicit refresh_target_t(refresh_target_options_t options);

    std::unique_ptr<impl_t> impl_;

    friend std::shared_ptr<refresh_target_t> make_refresh_target(refresh_target_options_t options);
  };

  std::shared_ptr<refresh_target_t> make_refresh_target(refresh_target_options_t options);

  // RTX HDR and other consumers use the same recent foreground sample when a refresh
  // target is already polling this capture rectangle. Falls back to a direct snapshot.
  foreground_app::state_t foreground_snapshot(const std::optional<RECT> &capture_rect = std::nullopt);

}  // namespace platf::game_activity

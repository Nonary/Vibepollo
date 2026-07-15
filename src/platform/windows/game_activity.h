/**
 * @file src/platform/windows/game_activity.h
 * @brief Shared prioritized game-activity signals for virtual-display policy consumers.
 */
#pragma once

#include "foreground_app.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <winsock2.h>
#include <windows.h>

namespace platf::game_activity {

  enum class signal_source_e : std::uint8_t {
    none = 0,
    fullscreen_foreground = 10,
    tracked_process = 20,
    playnite = 30,
  };

  struct signal_t {
    signal_source_e source {signal_source_e::none};
    bool active {false};
    DWORD pid {0};
    std::string executable;
  };

  struct state_t {
    bool active {false};
    signal_source_e source {signal_source_e::none};
    DWORD pid {0};
    std::string executable;
  };

  state_t reduce_signals(std::span<const signal_t> signals);
  const char *source_name(signal_source_e source);

  struct refresh_target_options_t {
    std::string display_name;
    std::string device_id;
    RECT capture_rect {};
    std::uint32_t base_refresh_numerator {0};
    std::uint32_t base_refresh_denominator {1};
    std::uint32_t high_refresh_numerator {0};
    std::uint32_t high_refresh_denominator {1};
    bool initial_high {false};
  };

  class refresh_target_t {
  public:
    ~refresh_target_t();

    refresh_target_t(const refresh_target_t &) = delete;
    refresh_target_t &operator=(const refresh_target_t &) = delete;

    // Called by WGC when DXGI reports a display change. If a refresh-only request is
    // in flight, wait for it to settle and consume permission for one soft refresh.
    bool wait_for_expected_refresh_change(std::chrono::milliseconds timeout);

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

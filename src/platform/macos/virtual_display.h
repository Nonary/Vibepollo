/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for the per-client virtual display on macOS.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace video {
  struct config_t;
}

/**
 * @brief Vibepollo's per-client virtual display on macOS, following the same settings as on Windows:
 *        virtual_display_mode, virtual_display_layout, and dd.virtual_display_scale_percent.
 */
namespace platf::macos_virtual_display {
  /**
   * @brief Create (or share) the virtual display for a stream and apply the configured layout.
   * @details While several streams are active they share one display, sized for the first of them.
   * @param config The stream's video configuration (client resolution and refresh rate).
   * @return A handle that keeps the display alive, or nullptr when the virtual display is disabled
   *         or couldn't be created, in which case the stream uses the physical display.
   */
  std::shared_ptr<void> acquire(const video::config_t &config);

  /**
   * @brief The virtual display that capture and input should target while one exists.
   */
  std::optional<std::uint32_t> active_display_id();

  /**
   * @brief Turn back on displays that a previous run turned off and never restored.
   * @details Schedules the work on the main event loop, so call it once at startup.
   */
  void recover_disabled_displays();

  /// Value of argv[1] that runs the process as the helper restoring displays if the main process dies.
  inline constexpr std::string_view restore_watchdog_arg = "--macos-display-restore-watchdog";

  /**
   * @brief Entry point for the restore helper (argv: executable, restore_watchdog_arg, display ids...).
   * @return Process exit code.
   */
  int run_restore_watchdog(int argc, char *argv[]);
}  // namespace platf::macos_virtual_display

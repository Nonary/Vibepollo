/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for the per-client virtual display on macOS.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video {
  struct config_t;
}

namespace remote_display_topology {
  struct mode_t;
  struct node_t;
}  // namespace remote_display_topology

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
   * @brief Bring up the virtual display when a client launches a stream, before the encoder probe.
   * @details Mirrors the Windows display helper, which applies the client's display before probing.
   *          A Mac without a screen of its own (a headless Mac mini, a MacBook with the lid closed)
   *          otherwise has nothing to probe. The hold ends when the stream's session acquires the
   *          display, when end_launch_hold() is called, or after 30 seconds.
   * @param config The client's requested resolution and refresh rate.
   */
  void hold_for_launch(const video::config_t &config);

  /**
   * @brief Release the hold from hold_for_launch(), if any: the session now holds the display,
   *        or the launch failed.
   */
  void end_launch_hold();

  /**
   * @brief The virtual display that capture and input should target while one exists.
   */
  std::optional<std::uint32_t> active_display_id();

  /**
   * @name Remote Monitor displays
   * @brief One virtual display per Remote Monitor client, arranged by the remote display topology.
   * @details These are the remote_display_topology runtime callbacks. A Remote Monitor display is
   *          captured by its display ID, which is also its name in platf::display_names().
   * @{
   */
  bool remote_create_or_reclaim(const std::string &client_uuid, const std::string &client_label, const remote_display_topology::mode_t &mode);
  void remote_resolve_mode(const std::string &client_uuid, remote_display_topology::mode_t &mode);
  bool remote_apply_composed_topology(const std::vector<remote_display_topology::node_t> &composed);
  std::optional<std::string> remote_exact_capture_output(const std::string &client_uuid, const remote_display_topology::mode_t &mode);
  bool remote_remove_owned_display(const std::string &client_uuid);

  /// The active displays other than Remote Monitor ones, which the topology arranges them around.
  std::vector<remote_display_topology::node_t> remote_baseline();

  /// Whether a display is one of the Remote Monitor displays.
  bool is_remote_display(std::uint32_t display_id);

  /// Whether a display is one of Vibepollo's virtual displays, made as an HDR display.
  bool is_hdr_display(std::uint32_t display_id);

  /**
   * @brief Record where a Remote Monitor display was when its capture started.
   * @details Its stream's input carries that origin as its touch port offset, which is how input
   *          finds the display even after the display moves.
   */
  void note_remote_capture_origin(std::uint32_t display_id, int x, int y);

  /// The Remote Monitor display whose capture started at an origin, from note_remote_capture_origin().
  std::optional<std::uint32_t> remote_display_captured_at(int x, int y);
  /** @} */

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

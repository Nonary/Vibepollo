/**
 * @file src/platform/windows/playnite_integration.h
 * @brief Playnite integration lifecycle and helper functions for Windows.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// third-party includes
#include <nlohmann/json.hpp>

// local includes
#include "src/platform/common.h"

namespace platf::playnite {

  /**
   * @brief Starts Playnite integration (IPC server + handlers).
   * @return A `std::unique_ptr<::platf::deinit_t>` guard that stops the integration on destruction.
   */
  [[nodiscard]] std::unique_ptr<::platf::deinit_t> start();

  /**
   * @brief Query whether an active IPC connection to Playnite exists.
   * @return `true` if connected to Playnite, `false` otherwise.
   */
  bool is_active();

  /**
   * @brief Ensure the IPC client is started for API access.
   * Call this before any API operation that needs fresh Playnite data.
   * The client will automatically stop after inactivity.
   */
  void ensure_client_for_api();

  /**
   * @brief Start the IPC client for a Playnite game session.
   * Call this when launching a Playnite-backed game.
   */
  void start_client_for_session();

  /**
   * @brief Stop the IPC client after a session ends.
   * Call this when a Playnite game session terminates.
   */
  void stop_client_for_session();

  /**
   * @brief Attempt to install the Playnite plugin in the default location.
   * @param[out] error Set to a human-readable error message on failure.
   * @return `true` on successful installation, `false` on failure.
   */
  bool install_plugin(std::string &error);

  /**
   * @brief Install the Playnite plugin to a specific destination directory.
   * @param[in] dest_dir Absolute path to the target installation directory.
   * @param[out] error Set to a human-readable error message on failure.
   * @return `true` on success, `false` on failure.
   */
  bool install_plugin_to(const std::string &dest_dir, std::string &error);
  /**
   * @brief Uninstall the Playnite plugin from the target Extensions directory.
   * @param[out] error Set to a human-readable error message on failure.
   * @return `true` on success (including when already uninstalled), `false` on failure.
   */
  bool uninstall_plugin(std::string &error);

  /**
   * @brief Compute the target extensions directory used for plugin installation.
   * @param[out] out Receives the absolute path to the target directory on success.
   * @return `true` if the directory was determined successfully, `false` otherwise.
   */
  bool get_extension_target_dir(std::string &out);

  /**
   * @brief Request Playnite to launch a game by its Playnite ID.
   * @param[in] playnite_id The Playnite identifier of the game to launch.
   * @return `true` if the launch request was sent or a fallback was triggered, `false` otherwise.
   */
  bool launch_game(const std::string &playnite_id);

  /**
   * @brief Announce a newly spawned launcher helper so the Playnite plugin can track it.
   * @param[in] pid Process identifier of the launcher helper (0 if unknown).
   * @param[in] game_id Associated Playnite game id (may be empty for fullscreen launches).
   * @return `true` if the announcement was sent to the plugin, `false` otherwise.
   */
  bool announce_launcher(uint32_t pid, const std::string &game_id);

  /**
   * @brief Request Playnite to stop/quit a running game by its Playnite ID.
   * @param[in] playnite_id The Playnite identifier of the game to stop.
   * @return `true` if the stop request was sent, `false` otherwise.
   */
  bool stop_game(const std::string &playnite_id);

  /**
   * @brief Get a JSON array string with minimal game info (`{id,name,categories}`).
   * @param[out] out_json Receives the JSON array string on success.
   * @return `true` if the game list data was available and written to `out_json`, `false` otherwise.
   */
  bool get_games_list_json(std::string &out_json);

  /**
   * @brief Get a JSON array string of categories.
   * Structure: [{"id":"...","name":"..."}]. When ids are not available,
   * objects may contain only the name field.
   * @param[out] out_json Receives the JSON array string on success.
   * @return `true` if category data was available and written to `out_json`, `false` otherwise.
   */
 bool get_categories_list_json(std::string &out_json);

  /**
   * @brief Get a JSON array string of library plugins (id + name).
   * @param[out] out_json Receives the JSON array string on success.
   * @return `true` if plugin data was available and written to `out_json`, `false` otherwise.
   */
  bool get_plugins_list_json(std::string &out_json);

  /**
   * @brief Force an immediate Playnite sync (applies auto-sync logic immediately).
   * @return `true` if the sync was triggered, `false` otherwise.
   */
  bool force_sync();

  /**
   * @brief Retrieve or generate a cover PNG for a Playnite game and return its path.
   * @param[in] playnite_id The Playnite identifier of the game.
   * @param[out] out_path Receives the filesystem path to the PNG on success.
   * @return `true` if a cover PNG path was obtained, `false` otherwise.
   */
  bool get_cover_png_for_playnite_game(const std::string &playnite_id, std::string &out_path);

  // no-op: persistence helper moved to confighttp as refresh_client_apps_cache

  /**
   * @brief Close any running Playnite instances and restart Playnite.
   * - When Sunshine runs as SYSTEM, launches Playnite by impersonating the active user.
   * - Otherwise, launches in the current session.
   * Attempts a graceful close and then forces termination if needed.
   * @return true on success (launch attempted), false on failure.
   */
  bool restart_playnite();

  // Note: explicit launch helper removed; use restart_playnite() for both cases.

  /**
   * @brief Read the Version from the packaged Playnite extension.yaml next to sunshine.exe.
   * @param[out] out Receives the version string on success.
   * @return true if the version was read successfully, false otherwise.
   */
  bool get_packaged_plugin_version(std::string &out);

  /**
   * @brief Read the Version from the installed Playnite extension.yaml under the user's Extensions directory.
   * @param[out] out Receives the version string on success.
   * @return true if the version was read successfully, false otherwise.
   */
  bool get_installed_plugin_version(std::string &out);

}  // namespace platf::playnite

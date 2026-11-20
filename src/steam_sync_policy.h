/** @file src/steam_sync_policy.h */
#pragma once

#include "config_playnite.h"
#include "steam_integration.h"

#include <nlohmann/json.hpp>
#include <vector>

namespace platf::steam::sync {
  namespace policy {
    std::string canonical_steam_app_uuid(std::uint32_t app_id);
    bool is_importable(const game_t &game, bool include_tools = false);
    std::vector<game_t> filter_games(const std::vector<game_t> &games, const std::vector<config::id_name_t> &exclusions, bool include_tools = false);
    std::vector<game_t> select_games(const std::vector<game_t> &games, bool sync_all_installed, int recent_games, int recent_max_age_days, const std::vector<config::id_name_t> &exclusions = {}, bool include_tools = false, std::uint64_t now = 0);

    struct status_counts_t {
      std::size_t game_count = 0;  ///< Installed games, including tools and excluded games.
      std::size_t tool_game_count = 0;  ///< Installed games filtered out as tools or runtimes.
      std::size_t excluded_game_count = 0;  ///< Installed importable games the user excluded.
      std::size_t selected_game_count = 0;  ///< Games the current sync mode would import.
    };

    // Summarize a discover_catalog() result for the status page. The selection
    // is exactly select_games(), so it sees the same play history as a real
    // sync; the other totals ignore uninstalled play-history entries.
    status_counts_t status_counts(const std::vector<game_t> &catalog, bool sync_all_installed, int recent_games, int recent_max_age_days, const std::vector<config::id_name_t> &exclusions = {}, bool include_tools = false, std::uint64_t now = 0);
    // Reconcile only entries marked steam-managed. Existing manual and
    // Playnite entries are retained untouched. Returns whether root changed.
    bool reconcile(nlohmann::json &root, const std::vector<game_t> &games, bool remove_uninstalled = true, const std::vector<config::id_name_t> &exclusions = {}, bool include_tools = false, const std::string &source = "installed");
  }  // namespace policy
}  // namespace platf::steam::sync

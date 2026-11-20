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
    // Reconcile only entries marked steam-managed. Existing manual and
    // Playnite entries are retained untouched. Returns whether root changed.
    bool reconcile(nlohmann::json &root, const std::vector<game_t> &games, bool remove_uninstalled = true, const std::vector<config::id_name_t> &exclusions = {}, bool include_tools = false, const std::string &source = "installed");
    // Summarize, for the host log, how the sync-owned entries differ between
    // two apps.json roots, e.g. "added 1 (Hades), removed 1 (Celeste); 10 Steam apps".
    std::string describe_changes(const nlohmann::json &before, const nlohmann::json &after);
    // The sync's selection policy for the host log, e.g. "10 most recent within 30 days".
    std::string describe_mode(bool sync_all_installed, int recent_games, int recent_max_age_days);
  }  // namespace policy
}  // namespace platf::steam::sync

/** @file src/steam_sync_policy.h */
#pragma once

#include "steam_integration.h"
#include "config_playnite.h"

#include <nlohmann/json.hpp>
#include <vector>

namespace platf::steam::sync {
  namespace policy {
    std::string canonical_steam_app_uuid(std::uint32_t app_id);
    bool is_importable(const game_t &game, bool include_tools = false);
    std::vector<game_t> filter_games(const std::vector<game_t> &games, const std::vector<config::id_name_t> &exclusions, bool include_tools = false);
    // Reconcile only entries marked steam-managed. Existing manual and
    // Playnite entries are retained untouched. Returns whether root changed.
    bool reconcile(nlohmann::json &root, const std::vector<game_t> &games, bool remove_uninstalled = true,
                   const std::vector<config::id_name_t> &exclusions = {}, bool include_tools = false);
  }  // namespace policy
}  // namespace platf::steam::sync

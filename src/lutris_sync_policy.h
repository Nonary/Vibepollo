/** @file src/lutris_sync_policy.h */
#pragma once

#include "config_playnite.h"
#include "lutris_integration.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace platf::lutris::sync::policy {
  std::string canonical_lutris_app_uuid(std::int64_t id);
  bool is_importable(const game_t &game, bool include_steam);
  std::vector<game_t> filter_games(const std::vector<game_t> &games,
                                   const std::vector<config::id_name_t> &exclusions,
                                   bool include_steam);
  bool reconcile(nlohmann::json &root, const std::vector<game_t> &games,
                 bool remove_uninstalled = true,
                 const std::vector<config::id_name_t> &exclusions = {},
                 bool include_steam = false);
}  // namespace platf::lutris::sync::policy

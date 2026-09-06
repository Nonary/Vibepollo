/** @file src/config_steam.h */
#pragma once

#include "config_playnite.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace config {
  struct steam_t {
    bool enabled = true;
    bool auto_sync = false;
    bool sync_all_installed = false;
    int recent_games = 10;
    int recent_max_age_days = 30;
    bool autosync_remove_uninstalled = true;
    // Compatibility/runtime manifests are excluded by default. This opt-in
    // exists for users who intentionally expose Steam tools in their catalog.
    bool include_tools = false;
    std::vector<std::string> exclude_games;
    std::vector<id_name_t> exclude_games_meta;
  };

  extern steam_t steam;

  // Parse provider settings without changing global state. Linux policy is
  // deliberately applied here so all callers (config/API/tests) agree.
  steam_t parse_steam(std::unordered_map<std::string, std::string> &vars);
  void apply_steam(std::unordered_map<std::string, std::string> &vars);
  steam_t normalize_steam_policy(steam_t value, bool linux_host);
}  // namespace config

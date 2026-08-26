/** @file src/config_lutris.h */
#pragma once

#include "config_playnite.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace config {
  struct lutris_t {
    bool enabled = true;
    bool auto_sync = true;
    bool autosync_remove_uninstalled = true;
    bool include_steam = false;
    std::vector<std::string> exclude_games;
    std::vector<id_name_t> exclude_games_meta;
  };

  extern lutris_t lutris;
  lutris_t parse_lutris(std::unordered_map<std::string, std::string> &vars);
  lutris_t normalize_lutris_policy(lutris_t value, bool linux_host);
  void apply_lutris(std::unordered_map<std::string, std::string> &vars);
}  // namespace config

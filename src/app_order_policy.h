/**
 * @file src/app_order_policy.h
 * @brief Pure ordering rules for the application list sent to clients.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace proc::app_order {
  enum class group_e {
    custom,
    steam,
    playnite,
    lutris,
  };

  enum class sort_e {
    name,
    recent,
    playtime,
  };

  struct settings_t {
    // Empty keeps the apps.json order unchanged.
    std::vector<group_e> groups;
    sort_e steam = sort_e::name;
    sort_e playnite = sort_e::name;
    sort_e lutris = sort_e::name;
  };

  struct entry_t {
    group_e group = group_e::custom;
    std::string name;
    std::int64_t last_played = 0;
    std::int64_t playtime_minutes = 0;
  };

  // Mirrors the web UI provider precedence: Playnite, then Steam, then Lutris.
  group_e group_of(std::string_view playnite_id, std::string_view steam_id, std::string_view lutris_id);

  // Parses the app_order_* config values. Unknown tokens are ignored.
  settings_t parse(std::string_view groups, std::string_view steam, std::string_view playnite, std::string_view lutris);

  // Returns indices into entries in client order. Listed groups come first in
  // the configured order; custom and unlisted apps keep their apps.json order.
  std::vector<std::size_t> order(const std::vector<entry_t> &entries, const settings_t &settings);
}  // namespace proc::app_order

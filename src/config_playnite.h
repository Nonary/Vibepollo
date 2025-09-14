/**
 * @file src/config_playnite.h
 * @brief Playnite integration configuration.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace config {

  struct id_name_t {
    std::string id;
    std::string name;
  };

  struct playnite_t {
    // Enabled flag removed; integration manager always runs (server up when plugin installed)
    bool auto_sync = true;  // enable automatic sync from Playnite
    int recent_games = 10;  // N most recent games
    // If > 0, only treat games whose last played time is within this many days
    // as "recent" for the purposes of recent-based auto-sync selection.
    int recent_max_age_days = 30;  // 0 = no age limit
    // Runtime: category names to sync (matched against game.categories)
    std::vector<std::string> sync_categories;
    // Persisted/meta: selected categories with id+name (for offline labeling)
    std::vector<id_name_t> sync_categories_meta;

    // Focus behavior
    int focus_attempts = 3;  // Count of confirmed re-applies of focus
    int focus_timeout_secs = 15;  // Total window to apply focus attempts
    bool focus_exit_on_first = false;  // Stop after first confirmed focus
    // Runtime: excluded Playnite game IDs
    std::vector<std::string> exclude_games;
    // Persisted/meta: excluded games with id+name (for offline labeling)
    std::vector<id_name_t> exclude_games_meta;

    // Auto-delete TTL for unplayed auto-synced apps (days). If > 0, an
    // auto-synced app that has not been launched from Playnite within this
    // many days of being added by auto-sync will be automatically removed.
    // 0 = remove immediately when it falls out of the selected set.
    int autosync_delete_after_days = 14;

    // When true, only purge auto-synced games that no longer qualify
    // if there is a qualifying replacement to fill the slot. When false,
    // always purge games that no longer qualify, even if it leaves fewer
    // than the configured recent slots.
    bool autosync_require_replacement = true;

    // Installation/paths (overrides removed)

    // When enabled, Sunshine maintains a "Playnite (Fullscreen)" entry in apps.json
    // that launches Playnite in fullscreen/desktop mode via the helper.
    bool fullscreen_entry_enabled = false;
  };

  extern playnite_t playnite;

  // Consume Playnite-related config vars from the provided map.
  void apply_playnite(std::unordered_map<std::string, std::string> &vars);
}  // namespace config

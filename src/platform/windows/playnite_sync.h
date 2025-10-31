/**
 * @file src/platform/windows/playnite_sync.h
 * @brief Small helpers for Playnite game selection and reconciliation.
 */
#pragma once

#include "src/confighttp.h"
#include "src/file_handler.h"
#include "src/platform/common.h"
#include "src/platform/windows/image_convert.h"
#include "src/platform/windows/playnite_protocol.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace platf::playnite::sync {

  // String helpers
  std::string to_lower_copy(std::string s);
  std::string normalize_path_for_match(const std::string &p);

  // Time helpers
  bool parse_iso8601_utc(const std::string &s, std::time_t &out);
  std::string now_iso8601_utc();

  // Selection helpers
  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed,
                                                  int recentN,
                                                  int recentAgeDays,
                                                  const std::unordered_set<std::string> &exclude_ids_lower,
                                                  const std::unordered_set<std::string> &exclude_categories_lower,
                                                  const std::unordered_set<std::string> &exclude_plugin_ids_lower,
                                                  std::unordered_map<std::string, int> &out_source_flags);

  std::vector<Game> select_category_games(const std::vector<Game> &installed,
                                          const std::vector<std::string> &categories,
                                          const std::unordered_set<std::string> &exclude_ids_lower,
                                          const std::unordered_set<std::string> &exclude_categories_lower,
                                          const std::unordered_set<std::string> &exclude_plugin_ids_lower,
                                          std::unordered_map<std::string, int> &out_source_flags);

  std::vector<Game> select_plugin_games(const std::vector<Game> &installed,
                                        const std::unordered_set<std::string> &plugins_lower,
                                        const std::unordered_set<std::string> &exclude_ids_lower,
                                        const std::unordered_set<std::string> &exclude_categories_lower,
                                        const std::unordered_set<std::string> &exclude_plugin_ids_lower,
                                        std::unordered_map<std::string, int> &out_source_flags);

  std::vector<Game> select_all_installed_games(const std::vector<Game> &installed,
                                               const std::unordered_set<std::string> &exclude_ids_lower,
                                               const std::unordered_set<std::string> &exclude_categories_lower,
                                               const std::unordered_set<std::string> &exclude_plugin_ids_lower,
                                               std::unordered_map<std::string, int> &out_source_flags);

  // Index helpers
  struct GameRef {
    const Game *g;
  };

  void build_game_indexes(const std::vector<Game> &selected, std::unordered_map<std::string, GameRef> &by_exe, std::unordered_map<std::string, GameRef> &by_dir, std::unordered_map<std::string, GameRef> &by_id);

  // Additional small helpers for apps.json reconciliation
  std::unordered_set<std::string> build_exclusion_lower(const std::vector<std::string> &ids);
  void snapshot_installed_and_uninstalled(const std::vector<Game> &all, std::vector<Game> &installed, std::unordered_set<std::string> &uninstalled_lower);
  std::unordered_map<std::string, std::time_t> build_last_played_map(const std::vector<Game> &installed);
  const Game *match_app_against_indexes(const nlohmann::json &app, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir);
  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app);
  void mark_app_as_playnite_auto(nlohmann::json &app, int flags);
  bool should_ttl_delete(const nlohmann::json &app, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played_map);
  void iterate_existing_apps(nlohmann::json &root, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, int> &source_flags, std::size_t &matched, std::unordered_set<std::string> &matched_ids, bool &changed);
  void add_missing_auto_entries(nlohmann::json &root, const std::vector<Game> &selected, const std::unordered_set<std::string> &matched_ids, const std::unordered_map<std::string, int> &source_flags, bool &changed);
  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path);

  // Purge helpers
  std::unordered_set<std::string> current_auto_ids(const nlohmann::json &root);
  std::size_t count_replacements_available(const std::unordered_set<std::string> &current_auto, const std::unordered_set<std::string> &selected_ids);
  void purge_uninstalled_and_ttl(nlohmann::json &root,
                                 const std::unordered_set<std::string> &uninstalled_lower,
                                 int delete_after_days,
                                 std::time_t now_time,
                                 const std::unordered_map<std::string, std::time_t> &last_played_map,
                                 bool recent_mode,
                                 bool require_repl,
                                 bool remove_uninstalled,
                                 bool sync_all_installed,
                                 const std::unordered_set<std::string> &selected_ids,
                                 bool &changed);

  // Orchestration helper: performs full autosync reconciliation into root["apps"].
  // Combines recent and category selections, merges source flags, purges, and adds missing entries.
  void autosync_reconcile(nlohmann::json &root,
                          const std::vector<Game> &all_games,
                          int recentN,
                          int recentAgeDays,
                          int delete_after_days,
                          bool require_repl,
                          bool sync_all_installed,
                          const std::vector<std::string> &categories,
                          const std::vector<std::string> &include_plugins,
                          const std::vector<std::string> &exclude_categories,
                          const std::vector<std::string> &exclude_ids,
                          const std::vector<std::string> &exclude_plugins,
                          bool remove_uninstalled,
                          bool &changed,
                          std::size_t &matched_out);

}  // namespace platf::playnite::sync

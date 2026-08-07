/**
 * @file src/platform/windows/playnite_sync_policy.h
 * @brief Deterministic Playnite synchronization decisions.
 *
 * This boundary deliberately accepts parsed game/app data rather than touching
 * the Playnite runtime, filesystem, HTTP cache, or image converter.
 */
#pragma once

#include "src/platform/windows/playnite_protocol.h"

#include <ctime>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace platf::playnite::sync {
  namespace policy {

  inline constexpr int kSourceRecent = 1 << 0;
  inline constexpr int kSourceCategory = 1 << 1;
  inline constexpr int kSourcePlugin = 1 << 2;
  inline constexpr int kSourceInstalled = 1 << 3;

  struct GameRef {
    const Game *g;
  };

  // The runtime supplies image/cache enrichment here. The reconciliation
  // policy itself only mutates parsed game and apps.json data.
  using MetadataUpdater = void (*)(const Game &game, nlohmann::json &app);

  std::string canonical_playnite_app_uuid(std::string_view playnite_id);
  std::string to_lower_copy(std::string s);
  std::string normalize_path_for_match(const std::string &p);
  std::string normalize_name_for_match(std::string_view s);
  std::string extract_cmd_executable_for_match(const std::string &cmd);
  bool parse_iso8601_utc(const std::string &s, std::time_t &out);

  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed, int recentN, int recent_age_days, std::time_t now_time, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed, int recentN, int recent_age_days, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_category_games(const std::vector<Game> &installed, const std::vector<std::string> &categories, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_plugin_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &plugins_lower, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_all_installed_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);

  // Whether image conversion must run, based solely on supplied cache state.
  bool should_reconvert_playnite_image(bool destination_exists, std::string_view recorded_source_signature, std::string_view source_signature);
  void apply_box_art_path(nlohmann::json &app, std::string_view converted_image_path);
  void apply_icon_path(nlohmann::json &app, std::string_view resolved_icon_path);

  bool should_ttl_delete(const nlohmann::json &app, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played_map);
  std::unordered_set<std::string> current_auto_ids(const nlohmann::json &root);
  std::size_t count_replacements_available(const std::unordered_set<std::string> &current_auto, const std::unordered_set<std::string> &selected_ids);
  void purge_uninstalled_and_ttl(nlohmann::json &root, const std::unordered_set<std::string> &uninstalled_lower, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played_map, bool recent_mode, bool require_repl, bool remove_uninstalled, bool sync_all_installed, const std::unordered_set<std::string> &selected_ids, bool &changed);

  std::string now_iso8601_utc();
  void build_game_indexes(const std::vector<Game> &selected, std::unordered_map<std::string, GameRef> &by_exe, std::unordered_map<std::string, GameRef> &by_dir, std::unordered_map<std::string, GameRef> &by_id, std::unordered_map<std::string, GameRef> &by_unique_name);
  std::unordered_set<std::string> build_exclusion_lower(const std::vector<std::string> &ids);
  void snapshot_installed_and_uninstalled(const std::vector<Game> &all, std::vector<Game> &installed, std::unordered_set<std::string> &uninstalled_lower);
  std::unordered_map<std::string, std::time_t> build_last_played_map(const std::vector<Game> &installed);
  const Game *match_app_against_indexes(const nlohmann::json &app, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, GameRef> &by_unique_name);
  void mark_app_as_playnite_auto(nlohmann::json &app, int flags);
  void add_missing_auto_entries(nlohmann::json &root, const std::vector<Game> &selected, const std::unordered_set<std::string> &matched_ids, const std::unordered_map<std::string, int> &source_flags, bool &changed, MetadataUpdater metadata_updater = nullptr);
  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, int recentN, int recent_age_days, int delete_after_days, bool require_replacement, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool &changed, std::size_t &matched_out, bool manage_membership = true, MetadataUpdater metadata_updater = nullptr);

  }  // namespace policy
}  // namespace platf::playnite::sync

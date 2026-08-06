/**
 * @file src/platform/windows/playnite_sync.h
 * @brief Small helpers for Playnite game selection and reconciliation.
 */
#pragma once

#include "src/platform/windows/playnite_sync_policy.h"

#include "src/confighttp.h"
#include "src/file_handler.h"
#include "src/platform/common.h"
#include "src/platform/windows/image_convert.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace platf::playnite::sync {

  std::string now_iso8601_utc();

  // Compatibility entry points for existing runtime callers. New isolated
  // policy tests use sync::policy and inject their clock explicitly.
  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed, int recentN, int recentAgeDays, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_category_games(const std::vector<Game> &installed, const std::vector<std::string> &categories, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_plugin_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &plugins_lower, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);
  std::vector<Game> select_all_installed_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &exclude_ids_lower, const std::unordered_set<std::string> &exclude_categories_lower, const std::unordered_set<std::string> &exclude_plugin_ids_lower, std::unordered_map<std::string, int> &out_source_flags);

  struct GameRef {
    const Game *g;
  };

  void build_game_indexes(const std::vector<Game> &selected, std::unordered_map<std::string, GameRef> &by_exe, std::unordered_map<std::string, GameRef> &by_dir, std::unordered_map<std::string, GameRef> &by_id, std::unordered_map<std::string, GameRef> &by_unique_name);
  std::unordered_set<std::string> build_exclusion_lower(const std::vector<std::string> &ids);
  void snapshot_installed_and_uninstalled(const std::vector<Game> &all, std::vector<Game> &installed, std::unordered_set<std::string> &uninstalled_lower);
  std::unordered_map<std::string, std::time_t> build_last_played_map(const std::vector<Game> &installed);
  const Game *match_app_against_indexes(const nlohmann::json &app, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, GameRef> &by_unique_name);

  // Art conversion cache: identity of the source image (path+size+mtime) that produced a converted PNG.
  std::string image_source_signature(const std::filesystem::path &src);
  bool convert_playnite_image_to_png(const std::string &src_path, const std::filesystem::path &dst);
  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app, const std::filesystem::path &covers_root);
  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app);
  void mark_app_as_playnite_auto(nlohmann::json &app, int flags);
  void iterate_existing_apps(nlohmann::json &root, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, GameRef> &by_unique_name, const std::unordered_map<std::string, int> &source_flags, std::size_t &matched, std::unordered_set<std::string> &matched_ids, bool &changed);
  void add_missing_auto_entries(nlohmann::json &root, const std::vector<Game> &selected, const std::unordered_set<std::string> &matched_ids, const std::unordered_map<std::string, int> &source_flags, bool &changed);
  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path);

  // Orchestration helper: refreshes linked Playnite app metadata and, when enabled,
  // reconciles automatic membership into root["apps"].
  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, int recentN, int recentAgeDays, int delete_after_days, bool require_repl, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool &changed, std::size_t &matched_out, bool manage_membership = true);

}  // namespace platf::playnite::sync

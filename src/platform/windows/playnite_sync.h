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

  // Art conversion cache: identity of the source image (path+size+mtime) that produced a converted PNG.
  std::string image_source_signature(const std::filesystem::path &src);
  bool convert_playnite_image_to_png(const std::string &src_path, const std::filesystem::path &dst);
  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app, const std::filesystem::path &covers_root);
  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app);
  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path);

  // Orchestration helper: refreshes linked Playnite app metadata and, when enabled,
  // reconciles automatic membership into root["apps"].
  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, int recentN, int recentAgeDays, int delete_after_days, bool require_repl, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool &changed, std::size_t &matched_out, bool manage_membership = true);

}  // namespace platf::playnite::sync

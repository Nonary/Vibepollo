/**
 * @file src/platform/windows/playnite_sync.cpp
 * @brief Runtime adapters for the portable Playnite sync policy.
 */

#include "playnite_sync.h"

#include "src/logging.h"
#include "src/platform/windows/playnite_integration.h"
#include "src/uuid.h"

#include <sstream>

namespace platf::playnite::sync {
  namespace {
    bool ensure_runtime_app_uuid(nlohmann::json &app) {
      try {
        if (app.contains("playnite-id") && app["playnite-id"].is_string() && !app["playnite-id"].get<std::string>().empty()) {
          return false;
        }
        const bool missing_uuid = !app.contains("uuid") || app["uuid"].is_null() || (app["uuid"].is_string() && app["uuid"].get<std::string>().empty());
        if (missing_uuid) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          return true;
        }
      } catch (...) {
        try {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          return true;
        } catch (...) {}
      }
      return false;
    }
  }  // namespace

  std::string image_source_signature(const std::filesystem::path &src) {
    std::error_code size_error;
    std::error_code time_error;
    const auto size = std::filesystem::file_size(src, size_error);
    const auto modified = std::filesystem::last_write_time(src, time_error);
    if (size_error || time_error) return {};
    std::ostringstream signature;
    signature << policy::normalize_path_for_match(src.string()) << '|' << size << '|' << modified.time_since_epoch().count();
    return signature.str();
  }

  bool convert_playnite_image_to_png(const std::string &src_path, const std::filesystem::path &dst) {
    if (src_path.empty()) return false;
    const auto src = std::filesystem::path(src_path);
    file_handler::make_directory(dst.parent_path().string());
    const auto signature = image_source_signature(src);
    const std::string sidecar = dst.string() + ".src";
    std::error_code exists_error;
    if (!policy::should_reconvert_playnite_image(std::filesystem::exists(dst, exists_error), file_handler::read_file(sidecar.c_str()), signature)) return true;
    if (!platf::img::convert_to_png_96dpi(src.wstring(), dst.wstring())) return false;
    if (!signature.empty()) file_handler::write_file(sidecar.c_str(), signature);
    return true;
  }

  void apply_game_metadata_to_app(const Game &game, nlohmann::json &app, const std::filesystem::path &covers_root) {
    try {
      if (!game.box_art_path.empty()) {
        const auto destination = covers_root / ("playnite_" + game.id + ".png");
        if (convert_playnite_image_to_png(game.box_art_path, destination)) policy::apply_box_art_path(app, destination.generic_string());
      }
    } catch (...) {}
    try {
      const auto destination = covers_root / ("playnite_icon_" + game.id + ".png");
      std::string install_dir = !game.install_dir.empty() ? game.install_dir : game.working_dir;
      if (install_dir.empty()) {
        std::string cached;
        if (platf::playnite::get_cached_install_dir(game.id, cached)) install_dir = cached;
      }
      platf::img::IconResolutionInfo diagnostics;
      if (platf::img::resolve_best_app_icon_png(std::filesystem::path(game.icon_path).wstring(), std::filesystem::path(game.exe).wstring(), std::filesystem::path(install_dir).wstring(), destination.wstring(), &diagnostics)) {
        policy::apply_icon_path(app, destination.generic_string());
        BOOST_LOG(debug) << "Playnite sync icon: name='" << game.name << "' installDir='" << install_dir << "' exeIcon=" << diagnostics.exe_size << " playniteIcon=" << diagnostics.icon_size << " -> width=" << platf::img::image_pixel_width(destination.wstring());
      } else {
        policy::apply_icon_path(app, {});
      }
    } catch (...) {}
  }

  void apply_game_metadata_to_app(const Game &game, nlohmann::json &app) {
    apply_game_metadata_to_app(game, app, platf::appdata() / "covers");
  }

  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path) {
    file_handler::write_file(apps_path.c_str(), root.dump(4));
    confighttp::refresh_client_apps_cache(root, false);
  }

  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, int recent_count, int recent_age_days, int delete_after_days, bool require_replacement, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool &changed, std::size_t &matched_out, bool manage_membership) {
    bool runtime_changed = false;
    if (root.contains("apps") && root["apps"].is_array()) {
      for (auto &app : root["apps"]) runtime_changed = ensure_runtime_app_uuid(app) || runtime_changed;
    }
    policy::autosync_reconcile(root, all_games, recent_count, recent_age_days, delete_after_days, require_replacement, sync_all_installed, categories, include_plugins, exclude_categories, exclude_ids, exclude_plugins, remove_uninstalled, changed, matched_out, manage_membership, static_cast<policy::MetadataUpdater>(&apply_game_metadata_to_app));
    changed = changed || runtime_changed;
  }
}  // namespace platf::playnite::sync

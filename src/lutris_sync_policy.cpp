#include "lutris_sync_policy.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace platf::lutris::sync::policy {
  namespace {
    nlohmann::json &apps(nlohmann::json &root) {
      if (!root.contains("apps") || !root["apps"].is_array()) root["apps"] = nlohmann::json::array();
      return root["apps"];
    }

    std::string lower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    }

    std::string id_of(const nlohmann::json &app) {
      if (app.contains("lutris-id") && app["lutris-id"].is_string()) return app["lutris-id"].get<std::string>();
      return {};
    }

    bool excluded(const game_t &game, const std::vector<config::id_name_t> &entries) {
      const auto id = std::to_string(game.id);
      for (const auto &entry : entries) {
        if (!entry.id.empty() && entry.id == id) return true;
        if (!entry.name.empty() && lower(entry.name) == lower(game.name)) return true;
      }
      return false;
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::string &value) {
      if (value.empty()) app.erase(key);
      else app[key] = value;
    }

    void update(nlohmann::json &app, const game_t &game) {
      app["name"] = game.name.empty() ? "Lutris " + std::to_string(game.id) : game.name;
      app["uuid"] = canonical_lutris_app_uuid(game.id);
      app["lutris-id"] = std::to_string(game.id);
      app["lutris-managed"] = "auto";
      set_or_erase(app, "lutris-slug", game.slug);
      set_or_erase(app, "lutris-runner", game.runner);
      set_or_erase(app, "lutris-platform", game.platform);
      set_or_erase(app, "lutris-directory", game.directory.generic_string());
      set_or_erase(app, "lutris-service", game.service);
      set_or_erase(app, "lutris-service-id", game.service_id);
      app["cmd"] = "lutris " + launch_uri(game.id);
      app["auto-detach"] = true;
      app["wait-all"] = false;
      if (!game.image_path.empty()) app["image-path"] = game.image_path.generic_string();
      else if (!app.contains("image-path") || !app["image-path"].is_string() ||
               app["image-path"].get<std::string>().empty()) app["image-path"] = "./assets/box.png";
    }
  }  // namespace

  std::string canonical_lutris_app_uuid(std::int64_t id) {
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setw(12) << std::setfill('0') << static_cast<std::uint64_t>(id);
    return "4c555452-4953-5000-8000-" + suffix.str();
  }

  bool is_importable(const game_t &game, bool include_steam) {
    return game.id > 0 && (include_steam || !game.steam_backed());
  }

  std::vector<game_t> filter_games(const std::vector<game_t> &games,
                                   const std::vector<config::id_name_t> &exclusions,
                                   bool include_steam) {
    std::vector<game_t> result;
    for (const auto &game : games) {
      if (is_importable(game, include_steam) && !excluded(game, exclusions)) result.push_back(game);
    }
    return result;
  }

  bool reconcile(nlohmann::json &root, const std::vector<game_t> &input_games,
                 bool remove_uninstalled,
                 const std::vector<config::id_name_t> &exclusions,
                 bool include_steam) {
    auto &entries = apps(root);
    bool changed = false;
    const auto games = filter_games(input_games, exclusions, include_steam);
    std::unordered_map<std::string, const game_t *> by_id;
    for (const auto &game : games) by_id.emplace(std::to_string(game.id), &game);
    std::unordered_set<std::string> non_importable_ids;
    for (const auto &game : input_games) {
      if (!is_importable(game, include_steam)) non_importable_ids.insert(std::to_string(game.id));
    }

    // Steam owns a shared Steam app ID unless the direct provider entry has
    // been excluded. Lutris metadata stays namespaced and never masquerades as
    // a direct Steam application.
    std::unordered_set<std::string> direct_steam_ids;
    for (const auto &app : entries) {
      if (app.contains("steam-id") && app["steam-id"].is_string() &&
          app.value("lutris-managed", std::string {}) != "auto") {
        direct_steam_ids.insert(app["steam-id"].get<std::string>());
      }
    }

    std::unordered_set<std::string> seen;
    for (auto iterator = entries.begin(); iterator != entries.end();) {
      auto &app = *iterator;
      const auto id = id_of(app);
      if (id.empty() || app.value("lutris-managed", std::string {}) != "auto") {
        ++iterator;
        continue;
      }
      const auto found = by_id.find(id);
      const bool is_excluded = std::any_of(exclusions.begin(), exclusions.end(), [&](const auto &entry) {
        return (!entry.id.empty() && entry.id == id) ||
               (!entry.name.empty() && lower(entry.name) == lower(app.value("name", std::string {})));
      });
      const bool shadowed_by_steam = found != by_id.end() && found->second->steam_backed() &&
                                     !found->second->service_id.empty() && direct_steam_ids.contains(found->second->service_id);
      if (is_excluded || shadowed_by_steam || non_importable_ids.contains(id) || seen.contains(id)) {
        iterator = entries.erase(iterator);
        changed = true;
        continue;
      }
      if (found != by_id.end()) {
        const auto before = app;
        update(app, *found->second);
        seen.insert(id);
        changed = changed || before != app;
        ++iterator;
      } else if (remove_uninstalled) {
        iterator = entries.erase(iterator);
        changed = true;
      } else {
        seen.insert(id);
        ++iterator;
      }
    }

    for (const auto &game : games) {
      const auto id = std::to_string(game.id);
      if (seen.contains(id)) continue;
      if (game.steam_backed() && !game.service_id.empty() && direct_steam_ids.contains(game.service_id)) continue;
      const auto existing = std::find_if(entries.begin(), entries.end(), [&](const auto &app) { return id_of(app) == id; });
      if (existing != entries.end()) continue;
      nlohmann::json app;
      update(app, game);
      entries.push_back(std::move(app));
      changed = true;
    }
    return changed;
  }
}  // namespace platf::lutris::sync::policy

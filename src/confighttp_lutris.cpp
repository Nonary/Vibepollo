/** @file src/confighttp_lutris.cpp */

#include "config.h"
#include "config_lutris.h"
#include "confighttp.h"
#include "file_handler.h"
#include "lutris_artwork.h"
#include "lutris_integration.h"
#include "lutris_sync_policy.h"
#include "platform/common.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <sstream>

namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  bool authenticate(resp_https_t response, req_https_t request);
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &content_type);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");

  namespace {
    bool is_excluded(const platf::lutris::game_t &game) {
      return std::any_of(config::lutris.exclude_games_meta.begin(), config::lutris.exclude_games_meta.end(),
                         [&game](const auto &entry) {
        return (!entry.id.empty() && entry.id == std::to_string(game.id)) ||
               (entry.id.empty() && !entry.name.empty() && entry.name == game.name);
      });
    }

    nlohmann::json game_json(const platf::lutris::game_t &game) {
      return {
        {"id", std::to_string(game.id)}, {"lutris_id", std::to_string(game.id)},
        {"stable_id", game.stable_id}, {"name", game.name}, {"slug", game.slug},
        {"runner", game.runner}, {"platform", game.platform},
        {"directory", game.directory.generic_string()}, {"config_path", game.config_path},
        {"service", game.service}, {"service_id", game.service_id},
        {"artwork_path", game.artwork_path.generic_string()},
        {"icon_path", game.icon_path.generic_string()},
        {"image_path", game.image_path.generic_string()}, {"steam_backed", game.steam_backed()},
        {"last_played", game.last_played}, {"playtime_seconds", game.playtime_seconds},
        {"launch_uri", platf::lutris::launch_uri(game.id)}, {"excluded", is_excluded(game)},
        {"filtered", !platf::lutris::sync::policy::is_importable(game, config::lutris.include_steam)},
      };
    }
  }  // namespace

  void getLutrisStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    try {
      const auto database = platf::lutris::default_database_path();
      const auto found = platf::lutris::discover(database);
      const auto importable = platf::lutris::sync::policy::filter_games(found, {}, config::lutris.include_steam);
      const auto filtered = platf::lutris::sync::policy::filter_games(
        found, config::lutris.exclude_games_meta, config::lutris.include_steam);
      nlohmann::json exclusions = nlohmann::json::array();
      for (const auto &entry : config::lutris.exclude_games_meta) {
        exclusions.push_back({{"id", entry.id}, {"name", entry.name}});
      }
      send_response(response, {
        {"status", true}, {"provider", "lutris"}, {"enabled", config::lutris.enabled},
        {"available", !database.empty() && platf::lutris::executable_available()},
        {"database_available", !database.empty()}, {"executable_available", platf::lutris::executable_available()},
        {"database_path", database.generic_string()}, {"game_count", found.size()},
        {"importable_game_count", filtered.size()}, {"steam_game_count", found.size() -
          platf::lutris::sync::policy::filter_games(found, {}, false).size()},
        {"excluded_game_count", importable.size() - filtered.size()}, {"exclude_games", std::move(exclusions)},
        {"auto_sync", config::lutris.auto_sync},
        {"autosync_remove_uninstalled", config::lutris.autosync_remove_uninstalled},
        {"include_steam", config::lutris.include_steam},
      });
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void getLutrisGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    try {
      nlohmann::json out {{"status", true}, {"enabled", config::lutris.enabled},
                          {"games", nlohmann::json::array()}};
      auto games = platf::lutris::discover();
      platf::lutris::artwork::prepare(games, platf::appdata());
      for (const auto &game : games) out["games"].push_back(game_json(game));
      send_response(response, out);
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void postLutrisForceSync(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json") || !authenticate(response, request)) return;
    if (!config::lutris.enabled) {
      bad_request(response, request, "Lutris integration is disabled");
      return;
    }
    try {
      const auto database = platf::lutris::default_database_path();
      if (database.empty() || !platf::lutris::executable_available()) {
        bad_request(response, request, "Lutris installation was not found");
        return;
      }
      auto found = platf::lutris::discover(database);
      platf::lutris::artwork::prepare(found, platf::appdata());
      std::lock_guard apps_lock {apps_file_mutex()};
      auto root = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      const bool changed = platf::lutris::sync::policy::reconcile(
        root, found, config::lutris.autosync_remove_uninstalled,
        config::lutris.exclude_games_meta, config::lutris.include_steam);
      if (changed && !refresh_client_apps_cache(root)) {
        bad_request(response, request, "Unable to save applications");
        return;
      }
      const auto filtered = platf::lutris::sync::policy::filter_games(
        found, config::lutris.exclude_games_meta, config::lutris.include_steam);
      send_response(response, {{"status", true}, {"changed", changed}, {"game_count", found.size()},
                               {"importable_game_count", filtered.size()}});
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void postLutrisLaunch(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json") || !authenticate(response, request)) return;
    if (!config::lutris.enabled) {
      bad_request(response, request, "Lutris integration is disabled");
      return;
    }
    try {
      std::stringstream stream;
      stream << request->content.rdbuf();
      const auto input = nlohmann::json::parse(stream.str());
      const auto value = input.contains("id") ? input["id"] : input.value("lutris_id", nlohmann::json {});
      std::int64_t id = 0;
      if (value.is_number_integer() || value.is_number_unsigned()) id = value.get<std::int64_t>();
      else if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character) {
              return std::isdigit(character) != 0;
            })) id = std::stoll(text);
      }
      const auto launchable = platf::lutris::sync::policy::filter_games(
        platf::lutris::discover(), config::lutris.exclude_games_meta, config::lutris.include_steam);
      const auto installed = std::find_if(launchable.begin(), launchable.end(), [id](const auto &game) {
        return game.id == id;
      });
      if (id <= 0 || installed == launchable.end() || !platf::lutris::launch(id)) {
        bad_request(response, request, "Lutris game is not installed, is excluded, or launch failed");
        return;
      }
      send_response(response, {{"status", true}, {"launch_uri", platf::lutris::launch_uri(id)}});
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }
}  // namespace confighttp

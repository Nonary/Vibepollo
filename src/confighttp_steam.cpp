/** @file src/confighttp_steam.cpp */

#include "config.h"
#include "config_steam.h"
#include "confighttp.h"
#include "file_handler.h"
#include "steam_integration.h"
#include "steam_artwork.h"
#include "steam_sync_policy.h"
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
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &contentType);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");

  namespace {
    nlohmann::json game_json(const platf::steam::game_t &game) {
      nlohmann::json out;
      out["appid"] = game.app_id;
      out["steam_id"] = std::to_string(game.app_id);
      out["stable_id"] = game.stable_id;
      out["name"] = game.name;
      out["install_dir"] = game.install_dir.generic_string();
      out["library_path"] = game.library_path.generic_string();
      out["icon_path"] = game.icon_path.generic_string();
      out["header_path"] = game.header_path.generic_string();
      out["portrait_path"] = game.portrait_path.generic_string();
      out["boxart_path"] = game.boxart_path.generic_string();
      out["artwork_path"] = game.artwork_path.generic_string();
      out["artwork_client_path"] = game.artwork_client_path.generic_string();
      out["artwork_format"] = game.artwork_format;
      out["artwork_client_compatible"] = !game.artwork_client_path.empty();
      out["app_type"] = game.app_type;
      out["importable"] = platf::steam::sync::policy::is_importable(game, config::steam.include_tools);
      out["launch_uri"] = platf::steam::launch_uri(game.app_id);
      return out;
    }
  }  // namespace

  void getSteamStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    try {
      const auto roots = platf::steam::default_library_roots();
      auto found = platf::steam::discover(roots);
      const auto importable = platf::steam::sync::policy::filter_games(found, {}, config::steam.include_tools);
      const auto filtered = platf::steam::sync::policy::filter_games(found, config::steam.exclude_games_meta, config::steam.include_tools);
      nlohmann::json exclusions = nlohmann::json::array();
      for (const auto &entry : config::steam.exclude_games_meta) {
        exclusions.push_back({{"id", entry.id}, {"name", entry.name}});
      }
      nlohmann::json out {{"status", true}, {"provider", "steam"}, {"enabled", config::steam.enabled},
                          {"forced", false}, {"available", !roots.empty()}, {"game_count", found.size()},
                          {"importable_game_count", filtered.size()},
                          {"tool_game_count", found.size() - importable.size()},
                          {"excluded_game_count", importable.size() - filtered.size()},
                          {"exclude_games", std::move(exclusions)}, {"auto_sync", config::steam.auto_sync},
                          {"autosync_remove_uninstalled", config::steam.autosync_remove_uninstalled},
                          {"include_tools", config::steam.include_tools}};
#if defined(__linux__)
      out["forced"] = true;
      out["playnite_available"] = false;
#else
      out["playnite_available"] = true;
#endif
      send_response(response, out);
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void getSteamGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    try {
      nlohmann::json out {{"status", true}, {"enabled", config::steam.enabled}, {"games", nlohmann::json::array()}};
      auto games = platf::steam::discover();
      platf::steam::artwork::prepare(games, platf::appdata());
      for (const auto &game : games) {
        auto item = game_json(game);
        item["excluded"] = std::find_if(config::steam.exclude_games_meta.begin(), config::steam.exclude_games_meta.end(), [&game](const auto &entry) {
          return (!entry.id.empty() && entry.id == std::to_string(game.app_id)) ||
                 (entry.id.empty() && !entry.name.empty() && entry.name == game.name);
        }) != config::steam.exclude_games_meta.end();
        item["filtered"] = !platf::steam::sync::policy::is_importable(game, config::steam.include_tools);
        out["games"].push_back(std::move(item));
      }
      send_response(response, out);
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void postSteamForceSync(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    if (!config::steam.enabled) {
      bad_request(response, request, "Steam integration is disabled");
      return;
    }
    try {
      const auto roots = platf::steam::default_library_roots();
      if (roots.empty()) {
        bad_request(response, request, "Steam installation was not found");
        return;
      }
      auto found = platf::steam::discover(roots);
      platf::steam::artwork::prepare(found, platf::appdata());
      std::lock_guard apps_lock {apps_file_mutex()};
      auto root = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      const bool changed = platf::steam::sync::policy::reconcile(root, found, config::steam.autosync_remove_uninstalled,
                                                                  config::steam.exclude_games_meta, config::steam.include_tools);
      if (changed && !refresh_client_apps_cache(root)) {
        bad_request(response, request, "Unable to save applications");
        return;
      }
      const auto filtered = platf::steam::sync::policy::filter_games(found, config::steam.exclude_games_meta, config::steam.include_tools);
      send_response(response, nlohmann::json {{"status", true}, {"changed", changed}, {"game_count", found.size()}, {"importable_game_count", filtered.size()}});
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }

  void postSteamLaunch(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    if (!config::steam.enabled) {
      bad_request(response, request, "Steam integration is disabled");
      return;
    }
    try {
      std::stringstream stream;
      stream << request->content.rdbuf();
      const auto input = nlohmann::json::parse(stream.str());
      std::uint64_t app_id = 0;
      const auto id = input.contains("appid") ? input["appid"] : input.value("steam_id", nlohmann::json {});
      if (id.is_number_unsigned() || id.is_number_integer()) {
        app_id = id.get<std::uint64_t>();
      } else if (id.is_string()) {
        const auto text = id.get<std::string>();
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
              return std::isdigit(c) != 0;
            })) {
          bad_request(response, request, "Steam app ID must be numeric");
          return;
        }
        app_id = std::stoull(text);
      }
      if (app_id == 0 || app_id > UINT32_MAX) {
        bad_request(response, request, "Invalid Steam app ID or Steam launch failed");
        return;
      }
      const auto requested_id = static_cast<std::uint32_t>(app_id);
      const auto launchable = platf::steam::sync::policy::filter_games(
        platf::steam::discover(), config::steam.exclude_games_meta, config::steam.include_tools);
      const auto installed = std::find_if(launchable.begin(), launchable.end(), [requested_id](const auto &game) {
        return game.app_id == requested_id;
      });
      if (installed == launchable.end() || !platf::steam::launch(requested_id)) {
        bad_request(response, request, "Steam app ID is not installed, is excluded, or Steam launch failed");
        return;
      }
      send_response(response, nlohmann::json {{"status", true}, {"launch_uri", platf::steam::launch_uri(static_cast<std::uint32_t>(app_id))}});
    } catch (const std::exception &error) {
      bad_request(response, request, error.what());
    }
  }
}  // namespace confighttp

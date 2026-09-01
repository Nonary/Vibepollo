#include "config_steam.h"

#include "config.h"
#ifndef SUNSHINE_TESTS
  #include "confighttp.h"
  #include "file_handler.h"
  #include "platform/common.h"
  #include "steam_artwork.h"
  #include "steam_auto_sync.h"
  #include "steam_integration.h"
  #include "steam_sync_policy.h"

  #include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sstream>

namespace config {
  steam_t steam;

  namespace {
    bool parse_bool(const std::string &raw, bool fallback) {
      std::string value = raw;
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
      }
      if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
      }
      return fallback;
    }

    void consume(std::unordered_map<std::string, std::string> &vars, const char *key, bool &result) {
      if (const auto it = vars.find(key); it != vars.end()) {
        result = parse_bool(it->second, result);
        vars.erase(it);
      }
    }

    void consume(std::unordered_map<std::string, std::string> &vars, const char *key, int &result) {
      if (const auto it = vars.find(key); it != vars.end()) {
        try {
          result = std::max(0, std::stoi(it->second));
        } catch (...) {
        }
        vars.erase(it);
      }
    }

    std::string take(std::unordered_map<std::string, std::string> &vars, const char *key) {
      const auto it = vars.find(key);
      if (it == vars.end()) {
        return {};
      }
      auto value = it->second;
      vars.erase(it);
      return value;
    }

    void parse_exclusions(std::unordered_map<std::string, std::string> &vars, steam_t &result) {
      const auto raw = take(vars, "steam_exclude_games");
      result.exclude_games.clear();
      result.exclude_games_meta.clear();
      if (raw.empty()) {
        return;
      }
      try {
        const auto json = nlohmann::json::parse(raw);
        if (json.is_array()) {
          for (const auto &item : json) {
            id_name_t entry;
            if (item.is_object()) {
              entry.id = item.value("id", std::string {});
              entry.name = item.value("name", std::string {});
            } else if (item.is_string()) {
              entry.id = item.get<std::string>();
            }
            if (!entry.id.empty() || !entry.name.empty()) {
              result.exclude_games.push_back(entry.id.empty() ? entry.name : entry.id);
              result.exclude_games_meta.push_back(std::move(entry));
            }
          }
          return;
        }
      } catch (...) {
      }
      std::stringstream stream(raw);
      for (std::string item; std::getline(stream, item, ',');) {
        const auto begin = item.find_first_not_of(" \t\r\n");
        const auto end = item.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos) {
          continue;
        }
        item = item.substr(begin, end - begin + 1);
        result.exclude_games.push_back(item);
        result.exclude_games_meta.push_back(id_name_t {item, {}});
      }
    }
  }  // namespace

  steam_t normalize_steam_policy(steam_t value, bool linux_host) {
    if (linux_host) {
      value.enabled = true;
    }
    return value;
  }

  steam_t parse_steam(std::unordered_map<std::string, std::string> &vars) {
    steam_t result;
    consume(vars, "steam_enabled", result.enabled);
    consume(vars, "steam_auto_sync", result.auto_sync);
    consume(vars, "steam_sync_all_installed", result.sync_all_installed);
    consume(vars, "steam_recent_games", result.recent_games);
    consume(vars, "steam_recent_max_age_days", result.recent_max_age_days);
    consume(vars, "steam_autosync_remove_uninstalled", result.autosync_remove_uninstalled);
    consume(vars, "steam_include_tools", result.include_tools);
    parse_exclusions(vars, result);
#if defined(__linux__)
    return normalize_steam_policy(result, true);
#else
    return normalize_steam_policy(result, false);
#endif
  }

  void apply_steam(std::unordered_map<std::string, std::string> &vars) {
    steam = parse_steam(vars);
#ifndef SUNSHINE_TESTS
    platf::steam::autosync::configure({
      .enabled = steam.enabled,
      .auto_sync = steam.auto_sync,
      .sync_all_installed = steam.sync_all_installed,
      .recent_games = steam.recent_games,
      .recent_max_age_days = steam.recent_max_age_days,
      .remove_uninstalled = steam.autosync_remove_uninstalled,
      .include_tools = steam.include_tools,
      .exclusions = steam.exclude_games_meta,
    });
    if (!steam.enabled || !steam.auto_sync) {
      return;
    }
    // Reconcile after the config is applied. Discovery is local and bounded;
    // failures are non-fatal because Steam is an optional installation on
    // Windows and may not have started yet.
    try {
      if (!platf::steam::available()) {
        return;
      }
      const auto path = config::stream.file_apps;
      std::lock_guard apps_lock {confighttp::apps_file_mutex()};
      const auto text = file_handler::read_file(path.c_str());
      if (text.empty()) {
        return;
      }
      auto root = nlohmann::json::parse(text);
      auto catalog = platf::steam::discover_catalog();
      auto games = platf::steam::sync::policy::select_games(
        catalog,
        steam.sync_all_installed,
        steam.recent_games,
        steam.recent_max_age_days
      );
      platf::steam::artwork::prepare(games, platf::appdata());
      const bool remove_missing = !steam.sync_all_installed || steam.autosync_remove_uninstalled;
      if (platf::steam::sync::policy::reconcile(root, games, remove_missing, steam.exclude_games_meta, steam.include_tools, steam.sync_all_installed ? "installed" : "recent")) {
        confighttp::refresh_client_apps_cache(root);
      }
    } catch (...) {
    }
#endif
  }
}  // namespace config

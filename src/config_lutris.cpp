#include "config_lutris.h"

#include "config.h"
#ifndef SUNSHINE_TESTS
  #include "confighttp.h"
  #include "file_handler.h"
  #include "lutris_artwork.h"
  #include "lutris_auto_sync.h"
  #include "lutris_integration.h"
  #include "lutris_sync_policy.h"
  #include "platform/common.h"

  #include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace config {
  lutris_t lutris;

  namespace {
    bool parse_bool(std::string value, bool fallback) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
      if (value == "0" || value == "false" || value == "no" || value == "off") return false;
      return fallback;
    }

    void consume_bool(std::unordered_map<std::string, std::string> &vars, const char *key, bool &value) {
      if (const auto iterator = vars.find(key); iterator != vars.end()) {
        value = parse_bool(iterator->second, value);
        vars.erase(iterator);
      }
    }

    void parse_exclusions(std::unordered_map<std::string, std::string> &vars, lutris_t &result) {
      const auto iterator = vars.find("lutris_exclude_games");
      if (iterator == vars.end()) return;
      const auto raw = iterator->second;
      vars.erase(iterator);
      if (raw.empty()) return;
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
      std::stringstream stream {raw};
      for (std::string item; std::getline(stream, item, ',');) {
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        item = item.substr(first, last - first + 1);
        result.exclude_games.push_back(item);
        result.exclude_games_meta.push_back({item, {}});
      }
    }
  }  // namespace

  lutris_t normalize_lutris_policy(lutris_t value, bool linux_host) {
    if (!linux_host) value.enabled = false;
    return value;
  }

  lutris_t parse_lutris(std::unordered_map<std::string, std::string> &vars) {
    lutris_t result;
    consume_bool(vars, "lutris_enabled", result.enabled);
    consume_bool(vars, "lutris_auto_sync", result.auto_sync);
    consume_bool(vars, "lutris_autosync_remove_uninstalled", result.autosync_remove_uninstalled);
    consume_bool(vars, "lutris_include_steam", result.include_steam);
    parse_exclusions(vars, result);
#if defined(__linux__)
    return normalize_lutris_policy(std::move(result), true);
#else
    return normalize_lutris_policy(std::move(result), false);
#endif
  }

  void apply_lutris(std::unordered_map<std::string, std::string> &vars) {
    lutris = parse_lutris(vars);
#ifndef SUNSHINE_TESTS
    platf::lutris::autosync::configure({
      .enabled = lutris.enabled,
      .auto_sync = lutris.auto_sync,
      .remove_uninstalled = lutris.autosync_remove_uninstalled,
      .include_steam = lutris.include_steam,
      .exclusions = lutris.exclude_games_meta,
    });
    if (!lutris.enabled || !lutris.auto_sync) return;
    try {
      if (!platf::lutris::discovery_ready()) return;
      auto games = platf::lutris::discover();
      platf::lutris::artwork::prepare(games, platf::appdata());
      std::lock_guard apps_lock {confighttp::apps_file_mutex()};
      const auto contents = file_handler::read_file(stream.file_apps.c_str());
      if (contents.empty()) return;
      auto root = nlohmann::json::parse(contents);
      if (platf::lutris::sync::policy::reconcile(root, games, lutris.autosync_remove_uninstalled,
                                                  lutris.exclude_games_meta, lutris.include_steam)) {
        confighttp::refresh_client_apps_cache(root);
      }
    } catch (...) {
    }
#endif
  }
}  // namespace config

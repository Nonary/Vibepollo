/**
 * @file src/config_playnite_parse.cpp
 * @brief Pure Playnite integration configuration parsing.
 */

#include "config_playnite.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace config {
  namespace {
    bool to_bool(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value == "true" || value == "yes" || value == "enable" || value == "enabled" || value == "on" || value == "1";
    }

    std::string erase_take(std::unordered_map<std::string, std::string> &vars, const std::string &name) {
      auto it = vars.find(name);
      if (it == vars.end()) {
        return {};
      }
      auto value = std::move(it->second);
      vars.erase(it);
      return value;
    }

    void parse_id_name_array(
      std::unordered_map<std::string, std::string> &vars,
      const std::string &name,
      std::vector<id_name_t> &metadata,
      std::vector<std::string> &values,
      bool strings_are_ids) {
      auto raw = erase_take(vars, name);
      metadata.clear();
      values.clear();
      if (raw.empty()) {
        return;
      }

      try {
        auto json = nlohmann::json::parse(raw);
        if (json.is_array()) {
          for (const auto &element : json) {
            id_name_t entry;
            if (element.is_object()) {
              entry.id = element.value("id", std::string {});
              entry.name = element.value("name", std::string {});
            } else if (element.is_string()) {
              const auto value = element.get<std::string>();
              if (strings_are_ids) {
                entry.id = value;
              } else {
                entry.name = value;
              }
            }
            if (entry.id.empty() && entry.name.empty()) {
              continue;
            }
            values.push_back(strings_are_ids ? entry.id : entry.name);
            metadata.push_back(std::move(entry));
          }
          return;
        }
      } catch (...) {
        // Non-JSON values retain the historical comma-separated fallback.
      }

      std::stringstream stream {raw};
      for (std::string item; std::getline(stream, item, ',');) {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char c) { return !std::isspace(c); }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), item.end());
        if (item.empty()) {
          continue;
        }
        id_name_t entry;
        if (strings_are_ids) {
          entry.id = item;
        } else {
          entry.name = item;
        }
        values.push_back(item);
        metadata.push_back(std::move(entry));
      }
    }

    void consume_bool(std::unordered_map<std::string, std::string> &vars, const char *name, bool &target) {
      auto value = erase_take(vars, name);
      if (!value.empty()) {
        target = to_bool(std::move(value));
      }
    }

    void consume_int(
      std::unordered_map<std::string, std::string> &vars,
      const char *name,
      int &target,
      bool clamp_nonnegative) {
      auto value = erase_take(vars, name);
      if (value.empty()) {
        return;
      }
      try {
        const auto parsed = std::stoi(value);
        target = clamp_nonnegative ? std::max(0, parsed) : parsed;
      } catch (...) {
        // Invalid values leave the production default unchanged.
      }
    }
  }  // namespace

  playnite_t parse_playnite(std::unordered_map<std::string, std::string> &vars) {
    playnite_t result;

    consume_bool(vars, "playnite_auto_sync", result.auto_sync);
    consume_bool(vars, "playnite_sync_all_installed", result.sync_all_installed);
    consume_bool(vars, "playnite_autosync_require_replacement", result.autosync_require_replacement);
    consume_bool(vars, "playnite_autosync_remove_uninstalled", result.autosync_remove_uninstalled);
    consume_bool(vars, "playnite_focus_exit_on_first", result.focus_exit_on_first);
    consume_bool(vars, "playnite_fullscreen_entry_enabled", result.fullscreen_entry_enabled);

    consume_int(vars, "playnite_recent_games", result.recent_games, false);
    consume_int(vars, "playnite_recent_max_age_days", result.recent_max_age_days, true);
    consume_int(vars, "playnite_autosync_delete_after_days", result.autosync_delete_after_days, true);
    consume_int(vars, "playnite_focus_attempts", result.focus_attempts, true);
    consume_int(vars, "playnite_focus_timeout_secs", result.focus_timeout_secs, true);

    parse_id_name_array(vars, "playnite_sync_categories", result.sync_categories_meta, result.sync_categories, false);
    parse_id_name_array(vars, "playnite_exclude_categories", result.exclude_categories_meta, result.exclude_categories, false);
    parse_id_name_array(vars, "playnite_sync_plugins", result.sync_plugins_meta, result.sync_plugins, true);
    parse_id_name_array(vars, "playnite_exclude_plugins", result.exclude_plugins_meta, result.exclude_plugins, true);
    parse_id_name_array(vars, "playnite_exclude_games", result.exclude_games_meta, result.exclude_games, true);
    return result;
  }
}  // namespace config

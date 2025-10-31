/**
 * @file src/config_playnite.cpp
 * @brief Playnite integration configuration parsing.
 */

#include "config_playnite.h"

#include "src/config.h"
#include "src/confighttp.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#ifdef _WIN32
  #include "src/platform/windows/playnite_integration.h"
#endif

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::literals;

namespace config {

  playnite_t playnite;

  static bool to_bool(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return (char) std::tolower(c);
    });
    return s == "true" || s == "yes" || s == "enable" || s == "enabled" || s == "on" || s == "1";
  }

  static void erase_take(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &out) {
    auto it = vars.find(name);
    if (it == vars.end()) {
      return;
    }
    out = std::move(it->second);
    vars.erase(it);
  }

  static void parse_id_name_array(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<config::id_name_t> &out, std::vector<std::string> *names_out = nullptr, bool treat_strings_as_ids = false) {
    std::string raw;
    erase_take(vars, name, raw);
    // Always reset outputs before parsing to avoid stale/accumulated state
    if (names_out) {
      names_out->clear();
    }
    if (raw.empty()) {
      // No value present in config; leave 'out' as-is to allow caller to
      // decide whether to preserve prior state or reset to defaults.
      return;
    }
    try {
      auto j = nlohmann::json::parse(raw);
      if (j.is_array()) {
        out.clear();
        for (auto &el : j) {
          if (el.is_object()) {
            config::id_name_t v;
            v.id = el.value("id", std::string());
            v.name = el.value("name", std::string());
            if (!v.id.empty() || !v.name.empty()) {
              out.emplace_back(v);
              if (names_out) {
                names_out->push_back(treat_strings_as_ids ? v.id : v.name);
              }
            }
          } else if (el.is_string()) {
            const std::string s = el.get<std::string>();
            if (s.empty()) {
              continue;
            }
            config::id_name_t v;
            if (treat_strings_as_ids) {
              v.id = s;
              v.name = std::string();
            } else {
              v.name = s;
              v.id = std::string();
            }
            out.emplace_back(std::move(v));
            if (names_out) {
              names_out->push_back(treat_strings_as_ids ? s : s);
            }
          }
        }
        return;
      }
    } catch (...) {
      // not JSON, try CSV below
    }
    // CSV fallback
    out.clear();
    std::vector<std::string> tmp;
    {
      std::string item;
      std::stringstream ss(raw);
      while (std::getline(ss, item, ',')) {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
                     return !std::isspace(ch);
                   }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                     return !std::isspace(ch);
                   }).base(),
                   item.end());
        if (!item.empty()) {
          tmp.push_back(item);
        }
      }
    }
    for (auto &s : tmp) {
      config::id_name_t v;
      if (treat_strings_as_ids) {
        v.id = s;
      } else {
        v.name = s;
      }
      out.emplace_back(std::move(v));
      if (names_out) {
        names_out->push_back(s);
      }
    }
  }

  void apply_playnite(std::unordered_map<std::string, std::string> &vars) {
    // booleans
    std::string tmp;
    // enabled flag removed; integration manager always runs and uses plugin install status
    tmp.clear();
    erase_take(vars, "playnite_auto_sync", tmp);
    if (!tmp.empty()) {
      playnite.auto_sync = to_bool(tmp);
    }
    tmp.clear();
    erase_take(vars, "playnite_sync_all_installed", tmp);
    if (!tmp.empty()) {
      playnite.sync_all_installed = to_bool(tmp);
    }
    tmp.clear();
    erase_take(vars, "playnite_autosync_require_replacement", tmp);
    if (!tmp.empty()) {
      playnite.autosync_require_replacement = to_bool(tmp);
    }
    tmp.clear();
    erase_take(vars, "playnite_autosync_remove_uninstalled", tmp);
    if (!tmp.empty()) {
      playnite.autosync_remove_uninstalled = to_bool(tmp);
    }

    // integers
    tmp.clear();
    erase_take(vars, "playnite_recent_games", tmp);
    if (!tmp.empty()) {
      try {
        playnite.recent_games = std::stoi(tmp);
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_recent_games value: " << tmp;
      }
    }
    // Recent max age (days): optional time-based filter for 'recent' selection
    tmp.clear();
    erase_take(vars, "playnite_recent_max_age_days", tmp);
    if (!tmp.empty()) {
      try {
        playnite.recent_max_age_days = std::max(0, std::stoi(tmp));
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_recent_max_age_days value: " << tmp;
      }
    }
    // New: delete-after for unplayed auto-synced apps (days)
    tmp.clear();
    erase_take(vars, "playnite_autosync_delete_after_days", tmp);
    if (!tmp.empty()) {
      try {
        playnite.autosync_delete_after_days = std::max(0, std::stoi(tmp));
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_autosync_delete_after_days value: " << tmp;
      }
    }

    tmp.clear();
    erase_take(vars, "playnite_focus_attempts", tmp);
    if (!tmp.empty()) {
      try {
        playnite.focus_attempts = std::max(0, std::stoi(tmp));
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_focus_attempts value: " << tmp;
      }
    }
    // Focus timeout (seconds)
    tmp.clear();
    erase_take(vars, "playnite_focus_timeout_secs", tmp);
    if (!tmp.empty()) {
      try {
        playnite.focus_timeout_secs = std::max(0, std::stoi(tmp));
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_focus_timeout_secs value: " << tmp;
      }
    }

    // Exit on first confirmed focus
    tmp.clear();
    erase_take(vars, "playnite_focus_exit_on_first", tmp);
    if (!tmp.empty()) {
      playnite.focus_exit_on_first = to_bool(tmp);
    }

    // Maintain Playnite fullscreen launcher entry in apps.json (Windows only)
    tmp.clear();
    erase_take(vars, "playnite_fullscreen_entry_enabled", tmp);
    if (!tmp.empty()) {
      playnite.fullscreen_entry_enabled = to_bool(tmp);
    }

    // lists
    // Reset lists to defaults first so removed keys clear runtime state on hot-apply
    playnite.sync_categories_meta.clear();
    playnite.sync_categories.clear();
    playnite.exclude_categories_meta.clear();
    playnite.exclude_categories.clear();
    playnite.sync_plugins_meta.clear();
    playnite.sync_plugins.clear();
    playnite.exclude_plugins_meta.clear();
    playnite.exclude_plugins.clear();
    playnite.exclude_games_meta.clear();
    playnite.exclude_games.clear();

    // Categories: accept JSON array of {id,name} or strings (names). Maintain runtime names list.
    parse_id_name_array(vars, "playnite_sync_categories", playnite.sync_categories_meta, &playnite.sync_categories, /*treat_strings_as_ids=*/false);
    // Excluded categories: accept JSON array of {id,name} or strings (names). Maintain runtime names list.
    parse_id_name_array(vars, "playnite_exclude_categories", playnite.exclude_categories_meta, &playnite.exclude_categories, /*treat_strings_as_ids=*/false);
    // Included plugins: accept JSON array of {id,name} or strings (ids). Maintain runtime ids list.
    parse_id_name_array(vars, "playnite_sync_plugins", playnite.sync_plugins_meta, &playnite.sync_plugins, /*treat_strings_as_ids=*/true);
    // Excluded plugins: accept JSON array of {id,name} or strings (ids). Maintain runtime ids list.
    parse_id_name_array(vars, "playnite_exclude_plugins", playnite.exclude_plugins_meta, &playnite.exclude_plugins, /*treat_strings_as_ids=*/true);
    // Exclusions: accept JSON array of {id,name} or strings (ids). Maintain runtime ids list.
    parse_id_name_array(vars, "playnite_exclude_games", playnite.exclude_games_meta, &playnite.exclude_games, /*treat_strings_as_ids=*/true);

    // paths (overrides removed)

#ifdef _WIN32
    try {
      const bool want = playnite.fullscreen_entry_enabled;
      // Read current apps list
      nlohmann::json file_tree = nlohmann::json::object();
      try {
        std::string content = file_handler::read_file(config::stream.file_apps.c_str());
        file_tree = nlohmann::json::parse(content);
      } catch (...) {
        file_tree["apps"] = nlohmann::json::array();
      }
      if (!file_tree.contains("apps") || !file_tree["apps"].is_array()) {
        file_tree["apps"] = nlohmann::json::array();
      }
      auto &apps = file_tree["apps"];
      auto is_fs = [](const nlohmann::json &app) -> bool {
        try {
          if (app.contains("playnite-fullscreen") && app["playnite-fullscreen"].is_boolean() && app["playnite-fullscreen"].get<bool>()) {
            return true;
          }
          if (app.contains("cmd") && app["cmd"].is_string()) {
            auto s = app["cmd"].get<std::string>();
            if (s.find("playnite-launcher") != std::string::npos && s.find("--fullscreen") != std::string::npos) {
              return true;
            }
          }
          if (app.contains("name") && app["name"].is_string()) {
            auto n = app["name"].get<std::string>();
            if (n == "Playnite (Fullscreen)") {
              return true;
            }
          }
        } catch (...) {}
        return false;
      };
      int idx = -1;
      for (size_t i = 0; i < apps.size(); ++i) {
        if (is_fs(apps[i])) {
          idx = static_cast<int>(i);
          break;
        }
      }
      bool changed = false;
      if (want && idx < 0) {
        nlohmann::json app;
        app["name"] = "Playnite (Fullscreen)";
        app["image-path"] = "playnite_boxart.png";
        app["playnite-fullscreen"] = true;
        app["auto-detach"] = true;
        app["wait-all"] = true;
        app["exit-timeout"] = 10;
        apps.push_back(std::move(app));
        changed = true;
      } else if (!want && idx >= 0) {
        nlohmann::json new_apps = nlohmann::json::array();
        for (size_t i = 0; i < apps.size(); ++i) {
          if (static_cast<int>(i) != idx) {
            new_apps.push_back(apps[i]);
          }
        }
        file_tree["apps"] = std::move(new_apps);
        changed = true;
      }
      if (changed) {
        confighttp::refresh_client_apps_cache(file_tree);
      }
    } catch (...) {
      // best-effort; ignore errors
    }
#endif
#ifdef _WIN32
    if (config::playnite.auto_sync) {
      try {
        platf::playnite::force_sync();
      } catch (...) {
      }
    }
#endif
  }
}  // namespace config

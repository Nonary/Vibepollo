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

  void apply_playnite(std::unordered_map<std::string, std::string> &vars) {
    playnite = parse_playnite(vars);

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
        // Startup/config application must not wait on an optional external plugin.
        // A delivered snapshot is reconciled asynchronously by the normal auto-sync path.
        platf::playnite::force_sync(false);
      } catch (...) {
      }
    }
#endif
  }
}  // namespace config

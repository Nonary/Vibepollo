/**
 * @file src/platform/windows/playnite_protocol.cpp
 */

#include "playnite_protocol.h"

#include "src/logging.h"

#include <nlohmann/json.hpp>
#include <string>

using nlohmann::json;

namespace platf::playnite {

  static std::vector<std::string> to_string_list(const json &j) {
    std::vector<std::string> out;
    if (!j.is_array()) {
      return out;
    }
    out.reserve(j.size());
    for (auto &v : j) {
      if (v.is_string()) {
        out.emplace_back(v.get<std::string>());
      }
    }
    return out;
  }

  Message parse(std::span<const uint8_t> bytes) {
    Message m;
    if (bytes.empty()) {
      return m;
    }
    std::span<const uint8_t> trimmed = bytes;
    if (trimmed.size() >= 3 && trimmed[0] == 0xEF && trimmed[1] == 0xBB && trimmed[2] == 0xBF) {
      trimmed = trimmed.subspan(3);
    }
    if (trimmed.empty()) {
      return m;
    }
    try {
      json j = json::parse(trimmed.begin(), trimmed.end());
      const std::string type = j.value("type", "");
      // Verbose protocol tracing: use debug to reduce noise in normal operation
      BOOST_LOG(debug) << "Playnite protocol: parsing message type='" << type << "'";
      if (type == "categories") {
        m.type = MessageType::Categories;
        auto arr = j.value("payload", json::array());
        BOOST_LOG(debug) << "Playnite protocol: categories count=" << arr.size();
        for (auto &c : arr) {
          Category cat;
          cat.id = c.value("id", "");
          cat.name = c.value("name", "");
          if (!cat.id.empty() || !cat.name.empty()) {
            m.categories.emplace_back(std::move(cat));
          }
        }
      } else if (type == "games") {
        m.type = MessageType::Games;
        auto arr = j.value("payload", json::array());
        BOOST_LOG(debug) << "Playnite protocol: games count=" << arr.size();
        for (auto &g : arr) {
          Game game;
          game.id = g.value("id", "");
          game.name = g.value("name", "");
          game.exe = g.value("exe", "");
          game.args = g.value("args", "");
          game.working_dir = g.value("workingDir", "");
          game.categories = to_string_list(g.value("categories", json::array()));
          // playtimeMinutes may arrive as number or string
          try {
            game.playtime_minutes = g.value("playtimeMinutes", (uint64_t) 0);
          } catch (...) {
            try {
              std::string pm = g.value("playtimeMinutes", std::string());
              if (!pm.empty()) {
                game.playtime_minutes = std::stoull(pm);
              }
            } catch (...) {}
          }
          game.last_played = g.value("lastPlayed", "");
          game.box_art_path = g.value("boxArtPath", "");
          game.description = g.value("description", "");
          game.tags = to_string_list(g.value("tags", json::array()));
          // Installed flag may be provided as 'installed' or 'isInstalled'.
          // If neither field is present, assume installed=true to avoid filtering out everything.
          bool has1 = g.contains("installed");
          bool has2 = g.contains("isInstalled");
          bool inst = false;
          if (has1) {
            inst = g.value("installed", false);
          }
          if (has2) {
            inst = inst || g.value("isInstalled", false);
          }
          if (!has1 && !has2) {
            inst = true;
          }
          game.installed = inst;
          if (!game.id.empty()) {
            m.games.emplace_back(std::move(game));
          }
        }
      } else if (type == "status") {
        m.type = MessageType::Status;
        const auto &st = j.value("status", json::object());
        m.status_name = st.value("name", "");
        m.status_game_id = st.value("id", "");
        m.status_install_dir = st.value("installDir", "");
        m.status_exe = st.value("exe", "");
        BOOST_LOG(debug) << "Playnite protocol: status name='" << m.status_name << "' id='" << m.status_game_id << "'";
      }
    } catch (...) {
      BOOST_LOG(warning) << "Playnite protocol: failed to parse message";
      // fallthrough unknown
    }
    return m;
  }

}  // namespace platf::playnite

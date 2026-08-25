#include "steam_sync_policy.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace platf::steam::sync::policy {
  namespace {
    nlohmann::json &apps(nlohmann::json &root) {
      if (!root.contains("apps") || !root["apps"].is_array()) {
        root["apps"] = nlohmann::json::array();
      }
      return root["apps"];
    }

    std::string lower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

    std::optional<std::uint32_t> parse_app_id(const std::string &value, int base = 10) {
      if (value.empty()) return std::nullopt;
      std::uint32_t result = 0;
      const auto *begin = value.data();
      const auto *end = begin + value.size();
      const auto parsed = std::from_chars(begin, end, result, base);
      if (parsed.ec != std::errc {} || parsed.ptr != end) return std::nullopt;
      return result;
    }

    std::optional<std::uint32_t> app_id_of(const nlohmann::json &app) {
      if (app.contains("steam-id")) {
        if (app["steam-id"].is_number_unsigned()) {
          const auto id = app["steam-id"].get<std::uint64_t>();
          if (id <= std::numeric_limits<std::uint32_t>::max()) return static_cast<std::uint32_t>(id);
        } else if (app["steam-id"].is_string()) {
          const auto raw = app["steam-id"].get<std::string>();
          if (const auto id = parse_app_id(raw)) return id;
          const auto normalized = lower(raw);
          if (normalized.starts_with("steam-")) return parse_app_id(raw.substr(6));
        }
      }
      if (app.contains("uuid") && app["uuid"].is_string()) {
        const auto uuid = app["uuid"].get<std::string>();
        const auto normalized = lower(uuid);
        if (normalized.starts_with("steam-")) return parse_app_id(uuid.substr(6));
        constexpr std::string_view canonical_prefix = "53544541-4d00-5000-8000-";
        if (normalized.starts_with(canonical_prefix) && normalized.size() == canonical_prefix.size() + 12) {
          return parse_app_id(normalized.substr(canonical_prefix.size()), 16);
        }
      }
      return std::nullopt;
    }

    std::string id_of(const nlohmann::json &app) {
      if (const auto id = app_id_of(app)) return std::to_string(*id);
      if (app.contains("steam-id") && app["steam-id"].is_string()) return app["steam-id"].get<std::string>();
      return {};
    }

    bool excluded(const game_t &game, const std::vector<config::id_name_t> &entries) {
      const auto id = std::to_string(game.app_id);
      for (const auto &entry : entries) {
        if (!entry.id.empty() && entry.id == id) return true;
        if (!entry.name.empty() && lower(entry.name) == lower(game.name)) return true;
      }
      return false;
    }

    bool excluded(const std::string &steam_id, const std::string &name, const std::vector<config::id_name_t> &entries) {
      for (const auto &entry : entries) {
        if (!entry.id.empty() && entry.id == steam_id) return true;
        if (!entry.name.empty() && lower(entry.name) == lower(name)) return true;
      }
      return false;
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::string &value) {
      if (value.empty()) app.erase(key);
      else app[key] = value;
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::filesystem::path &value) {
      set_or_erase(app, key, value.empty() ? std::string {} : value.generic_string());
    }

    void update(nlohmann::json &app, const game_t &game) {
      app["name"] = game.name.empty() ? "Steam " + std::to_string(game.app_id) : game.name;
      app["uuid"] = canonical_steam_app_uuid(game.app_id);
      app["steam-id"] = std::to_string(game.app_id);
      app["steam-managed"] = "auto";
      app["steam-source"] = "installed";
      set_or_erase(app, "steam-app-type", game.app_type);
#ifdef _WIN32
      app["cmd"] = "cmd /c start \"\" " + launch_uri(game.app_id);
#elif defined(__APPLE__)
      app["cmd"] = "open " + launch_uri(game.app_id);
#else
      app["cmd"] = "xdg-open " + launch_uri(game.app_id);
#endif
      app["auto-detach"] = true;
      app["wait-all"] = false;
      set_or_erase(app, "steam-install-dir", game.install_dir);
      set_or_erase(app, "steam-library-path", game.library_path);
      set_or_erase(app, "steam-icon-path", game.icon_path);
      set_or_erase(app, "steam-header-path", game.header_path);
      const auto &boxart = !game.boxart_path.empty() ? game.boxart_path : game.portrait_path;
      if (!boxart.empty()) {
        app["steam-boxart-path"] = boxart.generic_string();
      } else {
        app.erase("steam-boxart-path");
      }
      if (!game.artwork_path.empty()) {
        app["steam-artwork-path"] = game.artwork_path.generic_string();
        set_or_erase(app, "steam-artwork-format", game.artwork_format);
      } else {
        app.erase("steam-artwork-path");
        app.erase("steam-artwork-format");
      }
      if (!game.artwork_client_path.empty()) {
        app["steam-artwork-client-path"] = game.artwork_client_path.generic_string();
        app["steam-artwork-client-compatible"] = true;
        app["image-path"] = game.artwork_client_path.generic_string();
      } else {
        // Conversion failures and disappearing Steam cache files must not
        // leave stale compatibility metadata. If the prior image was our
        // managed cache, return to the built-in fallback; otherwise preserve
        // a user-selected custom image path.
        const auto previous_client = app.value("steam-artwork-client-path", std::string {});
        app.erase("steam-artwork-client-path");
        app.erase("steam-artwork-client-compatible");
        if (!previous_client.empty() && app.value("image-path", std::string {}) == previous_client) {
          app["image-path"] = "./assets/steam.png";
        } else if (!app.contains("image-path") || !app["image-path"].is_string() || app["image-path"].get<std::string>().empty()) {
          app["image-path"] = "./assets/steam.png";
        }
      }
    }

    void normalize_stale_identity(nlohmann::json &app, std::uint32_t app_id) {
      app["uuid"] = canonical_steam_app_uuid(app_id);
      app["steam-id"] = std::to_string(app_id);
      app["steam-managed"] = "auto";
    }
  }  // namespace

  std::string canonical_steam_app_uuid(std::uint32_t app_id) {
    // UUID-shaped, deterministic namespace for Steam app IDs. The first
    // fields encode "STEAM" while the final 48 bits carry the app ID.
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setw(12) << std::setfill('0') << app_id;
    return "53544541-4d00-5000-8000-" + suffix.str();
  }

  bool is_importable(const game_t &game, bool include_tools) {
    if (include_tools) return true;
    // Steam's manifest type is authoritative when present; these are not
    // games (redistributables, runtimes, configuration and DLC). Keep a small
    // ID allowlist for old manifests that omit `type`.
    const auto type = lower(game.app_type);
    if (type == "tool" || type == "config" || type == "dlc" || type == "driver" || type == "music" || type == "video") return false;
    static const std::unordered_set<std::uint32_t> runtime_ids {
      228980,   // Steamworks Common Redistributables
      1070560,  // Steam Linux Runtime
      1391110, 1628350, 1824220, 1493710, 4183110 // Soldier/Proton/runtime generations
    };
    if (runtime_ids.contains(game.app_id)) return false;
    // Older compatibility manifests commonly omit `type`. These narrow
    // provider-owned prefixes are a fallback, and steam_include_tools lets a
    // user explicitly override the classification.
    const auto name = lower(game.name);
    return !name.starts_with("proton ") &&
           !name.starts_with("steam linux runtime") &&
           name != "steamworks common redistributables";
  }

  std::vector<game_t> filter_games(const std::vector<game_t> &games, const std::vector<config::id_name_t> &exclusions, bool include_tools) {
    std::vector<game_t> result;
    for (const auto &game : games) {
      if (is_importable(game, include_tools) && !excluded(game, exclusions)) result.push_back(game);
    }
    return result;
  }

  bool reconcile(nlohmann::json &root, const std::vector<game_t> &input_games, bool remove_uninstalled,
                 const std::vector<config::id_name_t> &exclusions, bool include_tools) {
    auto &entries = apps(root);
    bool changed = false;
    const auto games = filter_games(input_games, exclusions, include_tools);
    std::unordered_map<std::string, const game_t *> by_id;
    for (const auto &game : games) {
      by_id.emplace(std::to_string(game.app_id), &game);
    }
    std::unordered_set<std::string> seen;
    for (auto it = entries.begin(); it != entries.end();) {
      auto &app = *it;
      const auto steam_id = id_of(app);
      if (steam_id.empty() || (!app.contains("steam-managed") || app["steam-managed"] != "auto")) {
        ++it;
        continue;
      }
      const auto name = app.value("name", std::string {});
      const bool is_excluded = excluded(steam_id, name, exclusions);
      if (is_excluded) {
        // Exclusions are an explicit catalog policy and remove stale
        // auto-managed entries even when uninstall removal is disabled.
        it = entries.erase(it);
        changed = true;
        continue;
      }
      if (const auto found = by_id.find(steam_id); found != by_id.end()) {
        if (seen.contains(steam_id)) {
          it = entries.erase(it);
          changed = true;
          continue;
        }
        const auto before = app;
        update(app, *found->second);
        seen.insert(steam_id);
        changed = changed || before != app;
        ++it;
      } else {
        if (seen.contains(steam_id)) {
          it = entries.erase(it);
          changed = true;
          continue;
        }
        seen.insert(steam_id);
        if (const auto stale_id = app_id_of(app)) {
          const auto before = app;
          normalize_stale_identity(app, *stale_id);
          changed = changed || before != app;
        }
        if (remove_uninstalled) {
          it = entries.erase(it);
          changed = true;
          continue;
        }
        ++it;
      }
    }
    for (const auto &game : games) {
      const auto key = std::to_string(game.app_id);
      if (seen.contains(key)) {
        continue;
      }
      // Do not duplicate a manually managed entry that has the same provider ID.
      const auto existing = std::find_if(entries.begin(), entries.end(), [&key](const auto &app) {
        return id_of(app) == key;
      });
      if (existing != entries.end()) {
        continue;
      }
      nlohmann::json app;
      update(app, game);
      entries.push_back(std::move(app));
      changed = true;
    }
    return changed;
  }
}  // namespace platf::steam::sync::policy

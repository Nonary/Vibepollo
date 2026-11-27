#include "steam_sync_policy.h"
#include "remote_monitor_profile_policy.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <limits>
#include <map>
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
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    std::optional<std::uint32_t> parse_app_id(const std::string &value, int base = 10) {
      if (value.empty()) {
        return std::nullopt;
      }
      std::uint32_t result = 0;
      const auto *begin = value.data();
      const auto *end = begin + value.size();
      const auto parsed = std::from_chars(begin, end, result, base);
      if (parsed.ec != std::errc {} || parsed.ptr != end) {
        return std::nullopt;
      }
      return result;
    }

    std::optional<std::uint32_t> app_id_of(const nlohmann::json &app) {
      if (app.contains("steam-id")) {
        if (app["steam-id"].is_number_unsigned()) {
          const auto id = app["steam-id"].get<std::uint64_t>();
          if (id <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(id);
          }
        } else if (app["steam-id"].is_string()) {
          const auto raw = app["steam-id"].get<std::string>();
          if (const auto id = parse_app_id(raw)) {
            return id;
          }
          const auto normalized = lower(raw);
          if (normalized.starts_with("steam-")) {
            return parse_app_id(raw.substr(6));
          }
        }
      }
      if (app.contains("uuid") && app["uuid"].is_string()) {
        const auto uuid = app["uuid"].get<std::string>();
        const auto normalized = lower(uuid);
        if (normalized.starts_with("steam-")) {
          return parse_app_id(uuid.substr(6));
        }
        constexpr std::string_view canonical_prefix = "53544541-4d00-5000-8000-";
        if (normalized.starts_with(canonical_prefix) && normalized.size() == canonical_prefix.size() + 12) {
          return parse_app_id(normalized.substr(canonical_prefix.size()), 16);
        }
      }
      return std::nullopt;
    }

    std::string id_of(const nlohmann::json &app) {
      if (const auto id = app_id_of(app)) {
        return std::to_string(*id);
      }
      if (app.contains("steam-id") && app["steam-id"].is_string()) {
        return app["steam-id"].get<std::string>();
      }
      return {};
    }

    bool has_canonical_steam_identity(const nlohmann::json &app,
                                      std::uint32_t app_id) {
      return app.contains("uuid") && app["uuid"].is_string() &&
             lower(app["uuid"].get<std::string>()) ==
               canonical_steam_app_uuid(app_id);
    }

    // Whether the sync owns this entry (callers skip Monitor profiles first).
    bool owned_by_sync(const nlohmann::json &app) {
      if (app.contains("steam-managed")) {
        return app["steam-managed"] == "auto";
      }
      // Early auto-imported entries already received Vibepollo's reserved,
      // deterministic Steam UUID but predate the steam-managed marker.  That
      // UUID is an ownership marker strong enough to adopt the entry.  Keep
      // shortcuts identified only by steam-id untouched as manual entries.
      const auto parsed_id = app_id_of(app);
      return parsed_id && has_canonical_steam_identity(app, *parsed_id);
    }

    // Sync-owned entries keyed by Steam app ID.
    std::map<std::string, const nlohmann::json *> owned_entries(const nlohmann::json &root) {
      std::map<std::string, const nlohmann::json *> result;
      if (!root.is_object() || !root.contains("apps") || !root["apps"].is_array()) {
        return result;
      }
      for (const auto &app : root["apps"]) {
        if (remote_session::marked_monitor_profile(app) || !owned_by_sync(app)) {
          continue;
        }
        if (auto id = id_of(app); !id.empty()) {
          result.emplace(std::move(id), &app);
        }
      }
      return result;
    }

    std::string name_list(const std::vector<std::string> &names) {
      std::string result;
      for (const auto &name : names) {
        if (!result.empty()) {
          result += ", ";
        }
        result += name;
      }
      return result;
    }

    bool excluded(const game_t &game, const std::vector<config::id_name_t> &entries) {
      const auto id = std::to_string(game.app_id);
      for (const auto &entry : entries) {
        if (!entry.id.empty() && entry.id == id) {
          return true;
        }
        if (!entry.name.empty() && lower(entry.name) == lower(game.name)) {
          return true;
        }
      }
      return false;
    }

    bool excluded(const std::string &steam_id, const std::string &name, const std::vector<config::id_name_t> &entries) {
      for (const auto &entry : entries) {
        if (!entry.id.empty() && entry.id == steam_id) {
          return true;
        }
        if (!entry.name.empty() && lower(entry.name) == lower(name)) {
          return true;
        }
      }
      return false;
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::string &value) {
      if (value.empty()) {
        app.erase(key);
      } else {
        app[key] = value;
      }
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::filesystem::path &value) {
      set_or_erase(app, key, value.empty() ? std::string {} : value.generic_string());
    }

    void update(nlohmann::json &app, const game_t &game, const std::string &source) {
      app["name"] = game.name.empty() ? "Steam " + std::to_string(game.app_id) : game.name;
      app["uuid"] = canonical_steam_app_uuid(game.app_id);
      app["steam-id"] = std::to_string(game.app_id);
      app["steam-managed"] = "auto";
      app["steam-source"] = source;
      set_or_erase(app, "steam-app-type", game.app_type);
      app["cmd"] = launch_command(game);
      app["auto-detach"] = true;
      app["wait-all"] = false;
      set_or_erase(app, "steam-install-dir", game.install_dir);
      set_or_erase(app, "steam-library-path", game.library_path);
      set_or_erase(app, "working-dir", game.launch_working_dir);
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
    if (include_tools) {
      return true;
    }
    // Steam's manifest type is authoritative when present; these are not
    // games (redistributables, runtimes, configuration and DLC). Keep a small
    // ID allowlist for old manifests that omit `type`.
    const auto type = lower(game.app_type);
    if (type == "tool" || type == "config" || type == "dlc" || type == "driver" || type == "music" || type == "video") {
      return false;
    }
    static const std::unordered_set<std::uint32_t> runtime_ids {
      228980,  // Steamworks Common Redistributables
      1070560,  // Steam Linux Runtime
      1391110,
      1628350,
      1824220,
      1493710,
      4183110  // Soldier/Proton/runtime generations
    };
    if (runtime_ids.contains(game.app_id)) {
      return false;
    }
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
      if (is_importable(game, include_tools) && !excluded(game, exclusions)) {
        result.push_back(game);
      }
    }
    return result;
  }

  std::vector<game_t> select_games(const std::vector<game_t> &games, bool sync_all_installed, int recent_games, int recent_max_age_days, const std::vector<config::id_name_t> &exclusions, bool include_tools, std::uint64_t now) {
    auto selected = filter_games(games, exclusions, include_tools);
    std::erase_if(selected, [sync_all_installed](const auto &game) {
      return !game.installed || (!sync_all_installed && game.last_played == 0);
    });
    if (sync_all_installed) {
      return selected;
    }
    if (recent_games <= 0) {
      return {};
    }
    if (now == 0) {
      now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
      )
                                         .count());
    }
    if (recent_max_age_days > 0) {
      const auto max_age = static_cast<std::uint64_t>(recent_max_age_days) * 24 * 60 * 60;
      selected.erase(std::remove_if(selected.begin(), selected.end(), [now, max_age](const auto &game) {
                       return game.last_played > now || now - game.last_played > max_age;
                     }),
                     selected.end());
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
      if (left.last_played != right.last_played) {
        return left.last_played > right.last_played;
      }
      return left.app_id < right.app_id;
    });
    if (selected.size() > static_cast<std::size_t>(recent_games)) {
      selected.resize(static_cast<std::size_t>(recent_games));
    }
    return selected;
  }

  bool reconcile(nlohmann::json &root, const std::vector<game_t> &input_games, bool remove_uninstalled, const std::vector<config::id_name_t> &exclusions, bool include_tools, const std::string &source) {
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
      // Explicit Monitor profiles retain their identity and configuration,
      // even when provider metadata or a legacy provider UUID is present.
      if (remote_session::marked_monitor_profile(app)) {
        ++it;
        continue;
      }
      const auto steam_id = id_of(app);
      if (steam_id.empty() || !owned_by_sync(app)) {
        // A manually added game may have been saved while artwork was offline.
        // Repair only provider-owned covers; leave its launch settings intact.
        if (const auto found = by_id.find(steam_id); found != by_id.end() &&
            !found->second->artwork_client_path.empty()) {
          const auto image = app.value("image-path", std::string {});
          const auto previous = app.value("steam-artwork-client-path", std::string {});
          if (image.empty() || image == "./assets/steam.png" || (!previous.empty() && image == previous)) {
            const auto before = app;
            const auto path = found->second->artwork_client_path.generic_string();
            app["image-path"] = path;
            app["steam-artwork-client-path"] = path;
            app["steam-artwork-client-compatible"] = true;
            changed = changed || app != before;
          }
        }
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
        update(app, *found->second, source);
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
      update(app, game, source);
      entries.push_back(std::move(app));
      changed = true;
    }
    return changed;
  }

  std::string describe_changes(const nlohmann::json &before, const nlohmann::json &after) {
    const auto old_entries = owned_entries(before);
    const auto new_entries = owned_entries(after);
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::size_t updated = 0;
    const auto label = [](const std::string &id, const nlohmann::json &app) {
      const auto name = app.contains("name") && app["name"].is_string() ? app["name"].get<std::string>() : std::string {};
      return name.empty() ? "app " + id : name;
    };
    for (const auto &[id, app] : new_entries) {
      if (const auto found = old_entries.find(id); found == old_entries.end()) {
        added.push_back(label(id, *app));
      } else if (*found->second != *app) {
        ++updated;
      }
    }
    for (const auto &[id, app] : old_entries) {
      if (!new_entries.contains(id)) {
        removed.push_back(label(id, *app));
      }
    }
    std::sort(added.begin(), added.end());
    std::sort(removed.begin(), removed.end());

    std::vector<std::string> parts;
    if (!added.empty()) {
      parts.push_back("added " + std::to_string(added.size()) + " (" + name_list(added) + ")");
    }
    if (!removed.empty()) {
      parts.push_back("removed " + std::to_string(removed.size()) + " (" + name_list(removed) + ")");
    }
    if (updated > 0) {
      parts.push_back("updated " + std::to_string(updated));
    }
    const auto summary = parts.empty() ? std::string {"no changes"} : name_list(parts);
    return summary + "; " + std::to_string(new_entries.size()) + " Steam apps";
  }

  std::string describe_mode(bool sync_all_installed, int recent_games, int recent_max_age_days) {
    if (sync_all_installed) {
      return "all installed";
    }
    auto mode = std::to_string(std::max(recent_games, 0)) + " most recent";
    if (recent_max_age_days > 0) {
      mode += " within " + std::to_string(recent_max_age_days) + " days";
    }
    return mode;
  }
}  // namespace platf::steam::sync::policy

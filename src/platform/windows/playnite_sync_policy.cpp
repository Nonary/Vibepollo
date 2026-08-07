#include "playnite_sync_policy.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace platf::playnite::sync::policy {
  namespace {
    std::string playnite_id_key(std::string_view id) {
      return to_lower_copy(std::string(id));
    }

    bool contains_playnite_id(const std::unordered_set<std::string> &ids, std::string_view id) {
      const auto raw_id = std::string(id);
      if (ids.contains(raw_id) || ids.contains(playnite_id_key(id))) {
        return true;
      }
      return std::any_of(ids.begin(), ids.end(), [&id](const auto &candidate) {
        return playnite_id_key(candidate) == playnite_id_key(id);
      });
    }

    const int *find_source_flags(const std::unordered_map<std::string, int> &source_flags, std::string_view id) {
      const auto raw_id = std::string(id);
      if (const auto found = source_flags.find(raw_id); found != source_flags.end()) {
        return &found->second;
      }
      if (const auto found = source_flags.find(playnite_id_key(id)); found != source_flags.end()) {
        return &found->second;
      }
      for (const auto &[candidate, flags] : source_flags) {
        if (playnite_id_key(candidate) == playnite_id_key(id)) {
          return &flags;
        }
      }
      return nullptr;
    }

    bool has_excluded_category(const Game &game, const std::unordered_set<std::string> &excluded) {
      return std::any_of(game.categories.begin(), game.categories.end(), [&excluded](const auto &category) {
        return excluded.contains(to_lower_copy(category));
      });
    }

    bool has_excluded_plugin(const Game &game, const std::unordered_set<std::string> &excluded) {
      return !game.plugin_id.empty() && excluded.contains(to_lower_copy(game.plugin_id));
    }

    bool is_excluded(const Game &game, const std::unordered_set<std::string> &ids, const std::unordered_set<std::string> &categories, const std::unordered_set<std::string> &plugins) {
      return (!game.id.empty() && ids.contains(playnite_id_key(game.id))) ||
             has_excluded_category(game, categories) || has_excluded_plugin(game, plugins);
    }

    void mark_source(std::unordered_map<std::string, int> &source_flags, const Game &game, int source) {
      const auto key = playnite_id_key(game.id);
      source_flags[key] |= source;
      // The production reconciler indexes flags case-insensitively. Retaining
      // the parsed id too makes this portable policy API convenient to callers
      // that keep Playnite's original id spelling.
      if (key != game.id) {
        source_flags[game.id] |= source;
      }
    }

    std::string compose_source_label(int flags) {
      if (flags == 0) {
        return "unknown";
      }
      std::vector<std::string> parts;
      if (flags & kSourceRecent) parts.emplace_back("recent");
      if (flags & kSourceCategory) parts.emplace_back("category");
      if (flags & kSourcePlugin) parts.emplace_back("plugin");
      if (flags & kSourceInstalled) parts.emplace_back("installed");
      std::string source;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) source.push_back('+');
        source += parts[index];
      }
      return source.empty() ? "unknown" : source;
    }

    void ensure_playnite_app_uuid(nlohmann::json &app, bool &changed) {
      try {
        if (!app.contains("playnite-id") || !app["playnite-id"].is_string()) {
          return;
        }
        const auto id = app["playnite-id"].get<std::string>();
        if (id.empty()) {
          return;
        }
        const auto uuid = canonical_playnite_app_uuid(id);
        if (!app.contains("uuid") || !app["uuid"].is_string() || app["uuid"].get<std::string>() != uuid) {
          app["uuid"] = uuid;
          changed = true;
        }
      } catch (...) {}
    }

    void apply_game_data(const Game &game, nlohmann::json &app) {
      try {
        if (!game.name.empty()) app["name"] = game.name;
        app["playnite-id"] = game.id;
        app.erase("cmd");
        app.erase("working-dir");
      } catch (...) {}
      try {
        if (game.plugin_id.empty()) {
          app.erase("playnite-plugin-id");
        } else {
          app["playnite-plugin-id"] = game.plugin_id;
        }
        if (game.plugin_name.empty()) {
          app.erase("playnite-plugin-name");
        } else {
          app["playnite-plugin-name"] = game.plugin_name;
        }
      } catch (...) {}
    }

    void dedupe_auto_apps_by_playnite_id(nlohmann::json &root, bool &changed) {
      if (!root.contains("apps") || !root["apps"].is_array()) {
        return;
      }
      nlohmann::json kept = nlohmann::json::array();
      std::unordered_set<std::string> seen;
      for (const auto &app : root["apps"]) {
        try {
          const auto auto_managed = app.value("playnite-managed", std::string {}) == "auto";
          const auto id = app.value("playnite-id", std::string {});
          if (auto_managed && !id.empty() && !seen.insert(playnite_id_key(id)).second) {
            changed = true;
            continue;
          }
        } catch (...) {}
        kept.push_back(app);
      }
      if (kept.size() != root["apps"].size()) {
        root["apps"] = std::move(kept);
      }
    }

    void iterate_existing_apps(nlohmann::json &root, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, GameRef> &by_unique_name, const std::unordered_map<std::string, int> &source_flags, const std::unordered_set<std::string> &uninstalled, std::size_t &matched, std::unordered_set<std::string> &matched_ids, bool &changed, MetadataUpdater metadata_updater) {
      for (auto &app : root["apps"]) {
        try {
          const auto is_auto = app.value("playnite-managed", std::string {}) == "auto";
          const auto id = app.value("playnite-id", std::string {});
          if (is_auto && !id.empty() && contains_playnite_id(uninstalled, id)) {
            continue;
          }
        } catch (...) {}
        const auto *game = match_app_against_indexes(app, by_id, by_exe, by_dir, by_unique_name);
        if (!game) {
          continue;
        }
        ensure_playnite_app_uuid(app, changed);
        ++matched;
        matched_ids.insert(playnite_id_key(game->id));
        apply_game_data(*game, app);
        if (metadata_updater) {
          metadata_updater(*game, app);
        }
        if (const auto *source = find_source_flags(source_flags, game->id)) {
          mark_app_as_playnite_auto(app, *source);
        }
        changed = true;
      }
    }
  }  // namespace

  std::string canonical_playnite_app_uuid(std::string_view playnite_id) {
    std::string uuid(playnite_id);
    std::transform(uuid.begin(), uuid.end(), uuid.begin(), [](unsigned char character) {
      return static_cast<char>(std::toupper(character));
    });
    return uuid;
  }

  std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return value;
  }

  std::string normalize_path_for_match(const std::string &path) {
    std::string normalized = path;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '"'), normalized.end());
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    return to_lower_copy(std::move(normalized));
  }

  std::string normalize_name_for_match(std::string_view name) {
    std::string normalized;
    bool pending_space = false;
    for (const auto character : name) {
      if (std::isspace(static_cast<unsigned char>(character))) {
        pending_space = !normalized.empty();
      } else {
        if (pending_space) {
          normalized.push_back(' ');
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        pending_space = false;
      }
    }
    return normalized;
  }

  std::string extract_cmd_executable_for_match(const std::string &command) {
    const auto start = command.find_first_not_of(" \t");
    if (start == std::string::npos) {
      return {};
    }
    if (command[start] == '"') {
      const auto end = command.find('"', start + 1);
      return normalize_path_for_match(command.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1));
    }
    const auto end = command.find_first_of(" \t", start);
    return normalize_path_for_match(command.substr(start, end == std::string::npos ? std::string::npos : end - start));
  }

  bool parse_iso8601_utc(const std::string &value, std::time_t &out) {
    if (value.empty()) {
      return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, sign = 0, offset_hour = 0, offset_minute = 0;
    std::size_t position = 0;
    const auto read_number = [&value, &position](int &destination, std::size_t length) {
      if (position + length > value.size()) {
        return false;
      }
      int parsed = 0;
      for (std::size_t index = 0; index < length; ++index) {
        const auto character = value[position + index];
        if (character < '0' || character > '9') {
          return false;
        }
        parsed = parsed * 10 + character - '0';
      }
      position += length;
      destination = parsed;
      return true;
    };
    if (!read_number(year, 4) || position >= value.size() || value[position++] != '-' || !read_number(month, 2) || position >= value.size() || value[position++] != '-' || !read_number(day, 2) || position >= value.size() || (value[position] != 'T' && value[position] != 't' && value[position] != ' ')) {
      return false;
    }
    ++position;
    if (!read_number(hour, 2) || position >= value.size() || value[position++] != ':' || !read_number(minute, 2) || position >= value.size() || value[position++] != ':' || !read_number(second, 2)) {
      return false;
    }
    if (position < value.size() && value[position] == '.') {
      ++position;
      while (position < value.size() && std::isdigit(static_cast<unsigned char>(value[position]))) {
        ++position;
      }
    }
    if (position < value.size()) {
      const auto zone = value[position];
      if (zone == 'Z' || zone == 'z') {
        ++position;
      } else if (zone == '+' || zone == '-') {
        sign = zone == '+' ? 1 : -1;
        ++position;
        if (!read_number(offset_hour, 2) || position >= value.size() || value[position++] != ':' || !read_number(offset_minute, 2)) {
          return false;
        }
      }
    }
    std::tm utc {};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    const auto parsed = _mkgmtime(&utc);
    if (parsed == static_cast<std::time_t>(-1)) {
      return false;
    }
    out = parsed - sign * (offset_hour * 3600L + offset_minute * 60L);
    return true;
  }

  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed, int recent_count, int recent_age_days, std::time_t now_time, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    auto ordered = installed;
    std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) { return left.last_played > right.last_played; });
    std::vector<Game> selected;
    const auto cutoff = now_time - static_cast<long long>(std::max(0, recent_age_days)) * 86400LL;
    for (const auto &game : ordered) {
      if (static_cast<int>(selected.size()) >= recent_count) {
        break;
      }
      if (is_excluded(game, excluded_ids, excluded_categories, excluded_plugins)) {
        continue;
      }
      std::time_t last_played = 0;
      if (recent_age_days > 0 && (!parse_iso8601_utc(game.last_played, last_played) || last_played < cutoff)) {
        continue;
      }
      selected.push_back(game);
      mark_source(source_flags, game, kSourceRecent);
    }
    return selected;
  }

  std::vector<Game> select_category_games(const std::vector<Game> &installed, const std::vector<std::string> &categories, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    std::unordered_set<std::string> wanted;
    for (auto category : categories) {
      wanted.insert(to_lower_copy(std::move(category)));
    }
    std::vector<Game> selected;
    for (const auto &game : installed) {
      const bool matched = std::any_of(game.categories.begin(), game.categories.end(), [&wanted](const auto &category) { return wanted.contains(to_lower_copy(category)); });
      if (!is_excluded(game, excluded_ids, excluded_categories, excluded_plugins) && matched) {
        selected.push_back(game);
        mark_source(source_flags, game, kSourceCategory);
      }
    }
    return selected;
  }

  std::vector<Game> select_plugin_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &plugins, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    std::vector<Game> selected;
    for (const auto &game : installed) {
      if (!is_excluded(game, excluded_ids, excluded_categories, excluded_plugins) && !game.plugin_id.empty() && plugins.contains(to_lower_copy(game.plugin_id))) {
        selected.push_back(game);
        mark_source(source_flags, game, kSourcePlugin);
      }
    }
    return selected;
  }

  std::vector<Game> select_all_installed_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    std::vector<Game> selected;
    for (const auto &game : installed) {
      if (!is_excluded(game, excluded_ids, excluded_categories, excluded_plugins)) {
        selected.push_back(game);
        if (!game.id.empty()) {
          mark_source(source_flags, game, kSourceInstalled);
        }
      }
    }
    return selected;
  }

  bool should_reconvert_playnite_image(bool destination_exists, std::string_view recorded_signature, std::string_view source_signature) {
    return source_signature.empty() || !destination_exists || recorded_signature != source_signature;
  }

  void apply_box_art_path(nlohmann::json &app, std::string_view converted_image_path) {
    if (!converted_image_path.empty()) {
      app["image-path"] = converted_image_path;
    }
  }

  void apply_icon_path(nlohmann::json &app, std::string_view resolved_icon_path) {
    if (resolved_icon_path.empty()) {
      app.erase("playnite-icon-path");
    } else {
      app["playnite-icon-path"] = resolved_icon_path;
    }
  }

  bool should_ttl_delete(const nlohmann::json &app, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played) {
    if (delete_after_days <= 0) {
      return false;
    }
    std::string id;
    std::time_t added = now_time;
    try {
      id = app.value("playnite-id", std::string {});
      const auto added_at = app.value("playnite-added-at", std::string {});
      std::time_t parsed = 0;
      if (parse_iso8601_utc(added_at, parsed)) {
        added = parsed;
      }
    } catch (...) {}
    const auto deadline = added + static_cast<long long>(delete_after_days) * 86400LL;
    if (now_time < deadline) {
      return false;
    }
    if (const auto played = last_played.find(id); played != last_played.end() && played->second >= added) {
      return false;
    }
    if (const auto played = last_played.find(playnite_id_key(id)); played != last_played.end() && played->second >= added) {
      return false;
    }
    return !std::any_of(last_played.begin(), last_played.end(), [&id, added](const auto &entry) {
      return playnite_id_key(entry.first) == playnite_id_key(id) && entry.second >= added;
    });
  }

  std::unordered_set<std::string> current_auto_ids(const nlohmann::json &root) {
    std::unordered_set<std::string> ids;
    if (!root.contains("apps") || !root["apps"].is_array()) {
      return ids;
    }
    for (const auto &app : root["apps"]) {
      try {
        if (app.value("playnite-managed", std::string {}) == "auto") {
          const auto id = app.value("playnite-id", std::string {});
          if (!id.empty()) {
            ids.insert(id);
          }
        }
      } catch (...) {}
    }
    return ids;
  }

  std::size_t count_replacements_available(const std::unordered_set<std::string> &current_auto, const std::unordered_set<std::string> &selected_ids) {
    return static_cast<std::size_t>(std::count_if(selected_ids.begin(), selected_ids.end(), [&current_auto](const auto &id) {
      return !contains_playnite_id(current_auto, id);
    }));
  }

  void purge_uninstalled_and_ttl(nlohmann::json &root, const std::unordered_set<std::string> &uninstalled, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played, bool recent_mode, bool require_replacement, bool remove_uninstalled, bool sync_all_installed, const std::unordered_set<std::string> &selected_ids, bool &changed) {
    if (!root.contains("apps") || !root["apps"].is_array()) {
      return;
    }
    auto replacements = count_replacements_available(current_auto_ids(root), selected_ids);
    nlohmann::json kept = nlohmann::json::array();
    for (const auto &app : root["apps"]) {
      bool remove = false;
      try {
        const auto auto_managed = app.value("playnite-managed", std::string {}) == "auto";
        const auto id = app.value("playnite-id", std::string {});
        if (auto_managed && !id.empty()) {
          remove = (remove_uninstalled && contains_playnite_id(uninstalled, id)) || should_ttl_delete(app, delete_after_days, now_time, last_played);
          if (!remove && !sync_all_installed && !contains_playnite_id(selected_ids, id) && app.value("playnite-source", std::string {}) == "installed") {
            remove = true;
          }
          if (!remove && !contains_playnite_id(selected_ids, id) && recent_mode && require_replacement && replacements > 0) {
            --replacements;
            remove = true;
          }
        }
      } catch (...) {}
      if (remove) {
        changed = true;
      } else {
        kept.push_back(app);
      }
    }
    if (kept.size() != root["apps"].size()) {
      root["apps"] = std::move(kept);
    }
  }

  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed, int recent_count, int recent_age_days, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    return select_recent_installed_games(installed, recent_count, recent_age_days, std::time(nullptr), excluded_ids, excluded_categories, excluded_plugins, source_flags);
  }

  std::string now_iso8601_utc() {
    const auto now = std::time(nullptr);
    std::tm utc {};
    gmtime_s(&utc, &now);
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return result.str();
  }

  void build_game_indexes(const std::vector<Game> &selected, std::unordered_map<std::string, GameRef> &by_exe, std::unordered_map<std::string, GameRef> &by_dir, std::unordered_map<std::string, GameRef> &by_id, std::unordered_map<std::string, GameRef> &by_unique_name) {
    std::unordered_set<std::string> ambiguous_names;
    for (const auto &game : selected) {
      if (!game.exe.empty()) by_exe[normalize_path_for_match(game.exe)] = GameRef {&game};
      if (!game.working_dir.empty()) by_dir[normalize_path_for_match(game.working_dir)] = GameRef {&game};
      if (!game.id.empty()) by_id[playnite_id_key(game.id)] = GameRef {&game};
      const auto name = normalize_name_for_match(game.name);
      if (!name.empty() && !ambiguous_names.contains(name)) {
        if (const auto [existing, inserted] = by_unique_name.emplace(name, GameRef {&game}); !inserted) {
          by_unique_name.erase(existing);
          ambiguous_names.insert(name);
        }
      }
    }
  }

  std::unordered_set<std::string> build_exclusion_lower(const std::vector<std::string> &ids) {
    std::unordered_set<std::string> excluded;
    for (auto id : ids) excluded.insert(to_lower_copy(std::move(id)));
    return excluded;
  }

  void snapshot_installed_and_uninstalled(const std::vector<Game> &all, std::vector<Game> &installed, std::unordered_set<std::string> &uninstalled) {
    installed = all;
    for (const auto &game : all) {
      if (!game.installed && !game.id.empty()) uninstalled.insert(playnite_id_key(game.id));
    }
    installed.erase(std::remove_if(installed.begin(), installed.end(), [](const auto &game) { return !game.installed; }), installed.end());
  }

  std::unordered_map<std::string, std::time_t> build_last_played_map(const std::vector<Game> &installed) {
    std::unordered_map<std::string, std::time_t> last_played;
    for (const auto &game : installed) {
      std::time_t time = 0;
      if (!game.id.empty() && parse_iso8601_utc(game.last_played, time)) last_played[playnite_id_key(game.id)] = time;
    }
    return last_played;
  }

  const Game *match_app_against_indexes(const nlohmann::json &app, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, GameRef> &by_unique_name) {
    try {
      if (const auto id = app.value("playnite-id", std::string {}); !id.empty()) {
        if (const auto found = by_id.find(playnite_id_key(id)); found != by_id.end()) return found->second.g;
      }
    } catch (...) {}
    try {
      if (const auto command = app.value("cmd", std::string {}); !command.empty()) {
        if (const auto found = by_exe.find(extract_cmd_executable_for_match(command)); found != by_exe.end()) return found->second.g;
      }
    } catch (...) {}
    try {
      if (const auto working_dir = app.value("working-dir", std::string {}); !working_dir.empty()) {
        if (const auto found = by_dir.find(normalize_path_for_match(working_dir)); found != by_dir.end()) return found->second.g;
      }
    } catch (...) {}
    try {
      if (const auto name = app.value("name", std::string {}); !name.empty()) {
        if (const auto found = by_unique_name.find(normalize_name_for_match(name)); found != by_unique_name.end()) return found->second.g;
      }
    } catch (...) {}
    return nullptr;
  }

  void mark_app_as_playnite_auto(nlohmann::json &app, int flags) {
    try {
      app["playnite-source"] = compose_source_label(flags);
      app["playnite-managed"] = "auto";
    } catch (...) {}
  }

  void add_missing_auto_entries(nlohmann::json &root, const std::vector<Game> &selected, const std::unordered_set<std::string> &matched_ids, const std::unordered_map<std::string, int> &source_flags, bool &changed, MetadataUpdater metadata_updater) {
    for (const auto &game : selected) {
      if (contains_playnite_id(matched_ids, game.id)) continue;
      nlohmann::json app = nlohmann::json::object();
      apply_game_data(game, app);
      if (metadata_updater) metadata_updater(game, app);
      ensure_playnite_app_uuid(app, changed);
      const auto *source = find_source_flags(source_flags, game.id);
      mark_app_as_playnite_auto(app, source ? *source : 0);
      try { app["playnite-added-at"] = now_iso8601_utc(); } catch (...) {}
      try { app["exit-timeout"] = 10; } catch (...) {}
      root["apps"].push_back(std::move(app));
      changed = true;
    }
  }

  void autosync_reconcile(nlohmann::json &root, const std::vector<Game> &all_games, int recent_count, int recent_age_days, int delete_after_days, bool require_replacement, bool sync_all_installed, const std::vector<std::string> &categories, const std::vector<std::string> &include_plugins, const std::vector<std::string> &exclude_categories, const std::vector<std::string> &exclude_ids, const std::vector<std::string> &exclude_plugins, bool remove_uninstalled, bool &changed, std::size_t &matched_out, bool manage_membership, MetadataUpdater metadata_updater) {
    if (!root.contains("apps") || !root["apps"].is_array()) root["apps"] = nlohmann::json::array();
    changed = false;
    matched_out = 0;
    if (manage_membership) dedupe_auto_apps_by_playnite_id(root, changed);

    std::vector<Game> installed;
    std::unordered_set<std::string> uninstalled;
    snapshot_installed_and_uninstalled(all_games, installed, uninstalled);
    const auto excluded_ids = build_exclusion_lower(exclude_ids);
    const auto excluded_categories = build_exclusion_lower(exclude_categories);
    const auto excluded_plugins = build_exclusion_lower(exclude_plugins);
    std::unordered_map<std::string, int> source_flags;
    std::vector<Game> recent, category, plugin, all_installed;
    if (recent_count > 0) recent = select_recent_installed_games(installed, recent_count, recent_age_days, std::time(nullptr), excluded_ids, excluded_categories, excluded_plugins, source_flags);
    if (!categories.empty()) category = select_category_games(installed, categories, excluded_ids, excluded_categories, excluded_plugins, source_flags);
    const auto included_plugins = build_exclusion_lower(include_plugins);
    if (!included_plugins.empty()) plugin = select_plugin_games(installed, included_plugins, excluded_ids, excluded_categories, excluded_plugins, source_flags);
    if (sync_all_installed) all_installed = select_all_installed_games(installed, excluded_ids, excluded_categories, excluded_plugins, source_flags);

    std::unordered_map<std::string, const Game *> selected_by_id;
    const auto merge = [&selected_by_id](const std::vector<Game> &games) {
      for (const auto &game : games) {
        if (!game.id.empty()) selected_by_id.emplace(playnite_id_key(game.id), &game);
      }
    };
    merge(recent);
    merge(category);
    merge(plugin);
    merge(all_installed);
    std::vector<Game> selected;
    selected.reserve(selected_by_id.size());
    for (const auto &[id, game] : selected_by_id) selected.push_back(*game);
    if (!manage_membership) {
      selected.clear();
      source_flags.clear();
    }

    std::unordered_map<std::string, GameRef> by_exe, by_dir, by_id, by_unique_name;
    build_game_indexes(selected, by_exe, by_dir, by_id, by_unique_name);
    for (const auto &game : all_games) {
      if (!game.id.empty()) by_id[playnite_id_key(game.id)] = GameRef {&game};
    }
    std::unordered_set<std::string> matched_ids;
    iterate_existing_apps(root, by_id, by_exe, by_dir, by_unique_name, source_flags, uninstalled, matched_out, matched_ids, changed, metadata_updater);
    if (!manage_membership) return;

    std::unordered_set<std::string> selected_ids;
    for (const auto &game : selected) selected_ids.insert(playnite_id_key(game.id));
    purge_uninstalled_and_ttl(root, uninstalled, delete_after_days, std::time(nullptr), build_last_played_map(installed), recent_count > 0, require_replacement, remove_uninstalled, sync_all_installed, selected_ids, changed);
    add_missing_auto_entries(root, selected, matched_ids, source_flags, changed, metadata_updater);
  }
}  // namespace platf::playnite::sync::policy

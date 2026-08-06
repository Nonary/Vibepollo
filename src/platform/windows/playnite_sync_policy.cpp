#include "playnite_sync_policy.h"

#include <algorithm>
#include <cctype>

namespace platf::playnite::sync::policy {
  namespace {
    std::string playnite_id_key(std::string_view id) {
      return to_lower_copy(std::string(id));
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
      source_flags[playnite_id_key(game.id)] |= kSourceRecent;
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
        source_flags[playnite_id_key(game.id)] |= kSourceCategory;
      }
    }
    return selected;
  }

  std::vector<Game> select_plugin_games(const std::vector<Game> &installed, const std::unordered_set<std::string> &plugins, const std::unordered_set<std::string> &excluded_ids, const std::unordered_set<std::string> &excluded_categories, const std::unordered_set<std::string> &excluded_plugins, std::unordered_map<std::string, int> &source_flags) {
    std::vector<Game> selected;
    for (const auto &game : installed) {
      if (!is_excluded(game, excluded_ids, excluded_categories, excluded_plugins) && !game.plugin_id.empty() && plugins.contains(to_lower_copy(game.plugin_id))) {
        selected.push_back(game);
        source_flags[playnite_id_key(game.id)] |= kSourcePlugin;
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
          source_flags[playnite_id_key(game.id)] |= kSourceInstalled;
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
    const auto played = last_played.find(playnite_id_key(id));
    return now_time >= added + static_cast<long long>(delete_after_days) * 86400LL && (played == last_played.end() || played->second < added);
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
            ids.insert(playnite_id_key(id));
          }
        }
      } catch (...) {}
    }
    return ids;
  }

  std::size_t count_replacements_available(const std::unordered_set<std::string> &current_auto, const std::unordered_set<std::string> &selected_ids) {
    return static_cast<std::size_t>(std::count_if(selected_ids.begin(), selected_ids.end(), [&current_auto](const auto &id) { return !current_auto.contains(id); }));
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
        const auto id_key = playnite_id_key(id);
        if (auto_managed && !id.empty()) {
          remove = (remove_uninstalled && uninstalled.contains(id_key)) || should_ttl_delete(app, delete_after_days, now_time, last_played);
          if (!remove && !sync_all_installed && !selected_ids.contains(id_key) && app.value("playnite-source", std::string {}) == "installed") {
            remove = true;
          }
          if (!remove && !selected_ids.contains(id_key) && recent_mode && require_replacement && replacements > 0) {
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
}  // namespace platf::playnite::sync::policy

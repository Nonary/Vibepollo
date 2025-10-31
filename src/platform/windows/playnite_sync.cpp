/**
 * @file src/platform/windows/playnite_sync.cpp
 */

#include "playnite_sync.h"

#include <iomanip>
#include <sstream>

namespace platf::playnite::sync {

  constexpr int kSourceRecent = 1 << 0;
  constexpr int kSourceCategory = 1 << 1;
  constexpr int kSourcePlugin = 1 << 2;
  constexpr int kSourceInstalled = 1 << 3;

  static std::string compose_source_label(int flags) {
    if (flags == 0) {
      return "unknown";
    }
    std::vector<std::string> parts;
    if (flags & kSourceRecent) {
      parts.emplace_back("recent");
    }
    if (flags & kSourceCategory) {
      parts.emplace_back("category");
    }
    if (flags & kSourcePlugin) {
      parts.emplace_back("plugin");
    }
    if (flags & kSourceInstalled) {
      parts.emplace_back("installed");
    }
    if (parts.empty()) {
      return "unknown";
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i != 0) {
        out.push_back('+');
      }
      out += parts[i];
    }
    return out;
  }

  std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return (char) std::tolower(c);
    });
    return s;
  }

  std::string normalize_path_for_match(const std::string &p) {
    std::string s = p;
    s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
    for (auto &c : s) {
      if (c == '/') {
        c = '\\';
      }
    }
    return to_lower_copy(s);
  }

  static bool game_has_excluded_category(const Game &g, const std::unordered_set<std::string> &exclude_categories_lower) {
    if (exclude_categories_lower.empty()) {
      return false;
    }
    for (const auto &cn : g.categories) {
      if (exclude_categories_lower.contains(to_lower_copy(cn))) {
        return true;
      }
    }
    return false;
  }

  static bool game_from_excluded_plugin(const Game &g, const std::unordered_set<std::string> &exclude_plugins_lower) {
    if (exclude_plugins_lower.empty()) {
      return false;
    }
    if (g.plugin_id.empty()) {
      return false;
    }
    return exclude_plugins_lower.contains(to_lower_copy(g.plugin_id));
  }

  bool parse_iso8601_utc(const std::string &s, std::time_t &out) {
    if (s.empty()) {
      return false;
    }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0, sg = 0, oh = 0, om = 0;
    size_t pos = 0;
    auto rd = [&](int &d, size_t n) {
      if (pos + n > s.size()) {
        return false;
      }
      int v = 0;
      for (size_t i = 0; i < n; ++i) {
        char c = s[pos + i];
        if (c < '0' || c > '9') {
          return false;
        }
        v = v * 10 + (c - '0');
      }
      pos += n;
      d = v;
      return true;
    };
    if (!rd(Y, 4) || pos >= s.size() || s[pos++] != '-' || !rd(M, 2) || pos >= s.size() || s[pos++] != '-' || !rd(D, 2) || pos >= s.size()) {
      return false;
    }
    if (!(s[pos] == 'T' || s[pos] == 't' || s[pos] == ' ')) {
      return false;
    }
    ++pos;
    if (!rd(h, 2) || pos >= s.size() || s[pos++] != ':' || !rd(m, 2) || pos >= s.size() || s[pos++] != ':' || !rd(sec, 2)) {
      return false;
    }
    if (pos < s.size() && s[pos] == '.') {
      ++pos;
      while (pos < s.size() && std::isdigit((unsigned char) s[pos])) {
        ++pos;
      }
    }
    if (pos < s.size()) {
      char c = s[pos];
      if (c == 'Z' || c == 'z') {
        ++pos;
      } else if (c == '+' || c == '-') {
        sg = (c == '+') ? 1 : -1;
        ++pos;
        if (!rd(oh, 2) || pos >= s.size() || s[pos++] != ':' || !rd(om, 2)) {
          return false;
        }
      }
    }
    std::tm tm {};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = sec;
    std::time_t t = _mkgmtime(&tm);
    if (t == (std::time_t) -1) {
      return false;
    }
    if (sg) {
      t -= (oh * 3600L + om * 60L) * sg;
    }
    out = t;
    return true;
  }

  std::string now_iso8601_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tm {};
    gmtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
  }

  std::vector<Game> select_recent_installed_games(const std::vector<Game> &installed,
                                                  int recentN,
                                                  int recentAgeDays,
                                                  const std::unordered_set<std::string> &exclude_lower,
                                                  const std::unordered_set<std::string> &exclude_categories_lower,
                                                  const std::unordered_set<std::string> &exclude_plugins_lower,
                                                  std::unordered_map<std::string, int> &out_source_flags) {
    std::vector<Game> v = installed;
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) {
      return a.last_played > b.last_played;
    });
    std::vector<Game> out;
    out.reserve((size_t) std::max(0, recentN));
    const std::time_t cutoff = std::time(nullptr) - (long long) std::max(0, recentAgeDays) * 86400LL;
    for (const auto &g : v) {
      if ((int) out.size() >= recentN) {
        break;
      }
      auto id = to_lower_copy(g.id);
      if (!id.empty() && exclude_lower.contains(id)) {
        continue;
      }
      if (game_has_excluded_category(g, exclude_categories_lower)) {
        continue;
      }
      if (game_from_excluded_plugin(g, exclude_plugins_lower)) {
        continue;
      }
      if (recentAgeDays > 0) {
        std::time_t tp = 0;
        if (!parse_iso8601_utc(g.last_played, tp) || tp < cutoff) {
          continue;
        }
      }
      out.push_back(g);
      out_source_flags[g.id] |= kSourceRecent;
    }
    return out;
  }

  std::vector<Game> select_plugin_games(const std::vector<Game> &installed,
                                        const std::unordered_set<std::string> &plugins_lower,
                                        const std::unordered_set<std::string> &exclude_lower,
                                        const std::unordered_set<std::string> &exclude_categories_lower,
                                        const std::unordered_set<std::string> &exclude_plugins_lower,
                                        std::unordered_map<std::string, int> &out_source_flags) {
    if (plugins_lower.empty()) {
      return {};
    }
    std::vector<Game> out;
    out.reserve(installed.size());
    for (const auto &g : installed) {
      auto id = to_lower_copy(g.id);
      if (!id.empty() && exclude_lower.contains(id)) {
        continue;
      }
      if (game_has_excluded_category(g, exclude_categories_lower)) {
        continue;
      }
      if (game_from_excluded_plugin(g, exclude_plugins_lower)) {
        continue;
      }
      if (g.plugin_id.empty()) {
        continue;
      }
      if (!plugins_lower.contains(to_lower_copy(g.plugin_id))) {
        continue;
      }
      out.push_back(g);
      out_source_flags[g.id] |= kSourcePlugin;
    }
    return out;
  }

  std::vector<Game> select_all_installed_games(const std::vector<Game> &installed,
                                               const std::unordered_set<std::string> &exclude_lower,
                                               const std::unordered_set<std::string> &exclude_categories_lower,
                                               const std::unordered_set<std::string> &exclude_plugins_lower,
                                               std::unordered_map<std::string, int> &out_source_flags) {
    std::vector<Game> out;
    out.reserve(installed.size());
    for (const auto &g : installed) {
      auto id = to_lower_copy(g.id);
      if (!id.empty() && exclude_lower.contains(id)) {
        continue;
      }
      if (game_has_excluded_category(g, exclude_categories_lower)) {
        continue;
      }
      if (game_from_excluded_plugin(g, exclude_plugins_lower)) {
        continue;
      }
      out.push_back(g);
      if (!g.id.empty()) {
        out_source_flags[g.id] |= kSourceInstalled;
      }
    }
    return out;
  }

  std::vector<Game> select_category_games(const std::vector<Game> &installed,
                                          const std::vector<std::string> &categories,
                                          const std::unordered_set<std::string> &exclude_lower,
                                          const std::unordered_set<std::string> &exclude_categories_lower,
                                          const std::unordered_set<std::string> &exclude_plugins_lower,
                                          std::unordered_map<std::string, int> &out_source_flags) {
    std::unordered_set<std::string> want;
    for (auto c : categories) {
      want.insert(to_lower_copy(std::move(c)));
    }
    std::vector<Game> out;
    out.reserve(installed.size());
    for (const auto &g : installed) {
      auto id = to_lower_copy(g.id);
      if (!id.empty() && exclude_lower.contains(id)) {
        continue;
      }
      if (game_has_excluded_category(g, exclude_categories_lower)) {
        continue;
      }
      if (game_from_excluded_plugin(g, exclude_plugins_lower)) {
        continue;
      }
      bool ok = false;
      for (const auto &cn : g.categories) {
        if (want.contains(to_lower_copy(cn))) {
          ok = true;
          break;
        }
      }
      if (ok) {
        out.push_back(g);
        out_source_flags[g.id] |= kSourceCategory;
      }
    }
    return out;
  }

  void build_game_indexes(const std::vector<Game> &selected, std::unordered_map<std::string, GameRef> &by_exe, std::unordered_map<std::string, GameRef> &by_dir, std::unordered_map<std::string, GameRef> &by_id) {
    for (const auto &g : selected) {
      if (!g.exe.empty()) {
        by_exe[normalize_path_for_match(g.exe)] = GameRef {&g};
      }
      if (!g.working_dir.empty()) {
        by_dir[normalize_path_for_match(g.working_dir)] = GameRef {&g};
      }
      if (!g.id.empty()) {
        by_id[g.id] = GameRef {&g};
      }
    }
  }

  std::unordered_set<std::string> build_exclusion_lower(const std::vector<std::string> &ids) {
    std::unordered_set<std::string> s;
    for (auto v : ids) {
      s.insert(to_lower_copy(std::move(v)));
    }
    return s;
  }

  void snapshot_installed_and_uninstalled(const std::vector<Game> &all, std::vector<Game> &installed, std::unordered_set<std::string> &uninstalled_lower) {
    installed = all;
    for (auto &g : all) {
      if (!g.installed && !g.id.empty()) {
        uninstalled_lower.insert(to_lower_copy(g.id));
      }
    }
    installed.erase(std::remove_if(installed.begin(), installed.end(), [](const auto &g) {
                      return !g.installed;
                    }),
                    installed.end());
  }

  std::unordered_map<std::string, std::time_t> build_last_played_map(const std::vector<Game> &installed) {
    std::unordered_map<std::string, std::time_t> m;
    for (const auto &g : installed) {
      std::time_t t = 0;
      if (!g.id.empty() && parse_iso8601_utc(g.last_played, t)) {
        m[g.id] = t;
      }
    }
    return m;
  }

  const Game *match_app_against_indexes(const nlohmann::json &app, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir) {
    try {
      if (app.contains("playnite-id") && app["playnite-id"].is_string()) {
        auto it = by_id.find(app["playnite-id"].get<std::string>());
        if (it != by_id.end()) {
          return it->second.g;
        }
      }
    } catch (...) {}
    try {
      if (app.contains("cmd") && app["cmd"].is_string()) {
        auto it = by_exe.find(normalize_path_for_match(app["cmd"].get<std::string>()));
        if (it != by_exe.end()) {
          return it->second.g;
        }
      }
    } catch (...) {}
    try {
      if (app.contains("working-dir") && app["working-dir"].is_string()) {
        auto it = by_dir.find(normalize_path_for_match(app["working-dir"].get<std::string>()));
        if (it != by_dir.end()) {
          return it->second.g;
        }
      }
    } catch (...) {}
    return nullptr;
  }

  void apply_game_metadata_to_app(const Game &g, nlohmann::json &app) {
    try {
      if (!g.name.empty()) {
        app["name"] = g.name;
      }
    } catch (...) {}
    try {
      if (!g.box_art_path.empty()) {
        auto dstDir = platf::appdata() / "covers";
        file_handler::make_directory(dstDir.string());
        auto dst = dstDir / ("playnite_" + g.id + ".png");
        bool ok = std::filesystem::exists(dst);
        if (!ok) {
          ok = platf::img::convert_to_png_96dpi(std::filesystem::path(g.box_art_path).wstring(), dst.wstring());
        }
        if (ok) {
          app["image-path"] = dst.generic_string();
        }
      }
    } catch (...) {}
    try {
      app["playnite-id"] = g.id;
      if (app.contains("cmd")) {
        app.erase("cmd");
      }
      if (app.contains("working-dir")) {
        app.erase("working-dir");
      }
    } catch (...) {}
    try {
      if (!g.plugin_id.empty()) {
        app["playnite-plugin-id"] = g.plugin_id;
      } else if (app.contains("playnite-plugin-id")) {
        app.erase("playnite-plugin-id");
      }
    } catch (...) {}
    try {
      if (!g.plugin_name.empty()) {
        app["playnite-plugin-name"] = g.plugin_name;
      } else if (app.contains("playnite-plugin-name")) {
        app.erase("playnite-plugin-name");
      }
    } catch (...) {}
  }

  void mark_app_as_playnite_auto(nlohmann::json &app, int flags) {
    try {
      app["playnite-source"] = compose_source_label(flags);
      app["playnite-managed"] = "auto";
    } catch (...) {}
  }

  bool should_ttl_delete(const nlohmann::json &app, int delete_after_days, std::time_t now_time, const std::unordered_map<std::string, std::time_t> &last_played_map) {
    if (delete_after_days <= 0) {
      return false;
    }
    std::string pid;
    try {
      if (app.contains("playnite-id")) {
        pid = app["playnite-id"].get<std::string>();
      }
    } catch (...) {}
    std::time_t added = 0;
    bool has = false;
    try {
      if (app.contains("playnite-added-at")) {
        has = parse_iso8601_utc(app["playnite-added-at"].get<std::string>(), added);
      }
    } catch (...) {}
    if (!has) {
      added = now_time;
    }
    auto it = last_played_map.find(pid);
    bool played = it != last_played_map.end() && it->second >= added;
    std::time_t deadline = added + (long long) delete_after_days * 86400LL;
    return now_time >= deadline && !played;
  }

  void iterate_existing_apps(nlohmann::json &root, const std::unordered_map<std::string, GameRef> &by_id, const std::unordered_map<std::string, GameRef> &by_exe, const std::unordered_map<std::string, GameRef> &by_dir, const std::unordered_map<std::string, int> &source_flags, std::size_t &matched, std::unordered_set<std::string> &matched_ids, bool &changed) {
    for (auto &app : root["apps"]) {
      auto g = match_app_against_indexes(app, by_id, by_exe, by_dir);
      if (!g) {
        continue;
      }
      ++matched;
      matched_ids.insert(g->id);
      apply_game_metadata_to_app(*g, app);
      int flags = 0;
      if (auto it = source_flags.find(g->id); it != source_flags.end()) {
        flags = it->second;
      }
      mark_app_as_playnite_auto(app, flags);
      changed = true;
    }
  }

  void add_missing_auto_entries(nlohmann::json &root, const std::vector<Game> &selected, const std::unordered_set<std::string> &matched_ids, const std::unordered_map<std::string, int> &source_flags, bool &changed) {
    for (const auto &g : selected) {
      if (matched_ids.contains(g.id)) {
        continue;
      }
      nlohmann::json app = nlohmann::json::object();
      apply_game_metadata_to_app(g, app);
      int flags = 0;
      if (auto it = source_flags.find(g.id); it != source_flags.end()) {
        flags = it->second;
      }
      mark_app_as_playnite_auto(app, flags);
      // stamp added-at for TTL
      try {
        app["playnite-added-at"] = nlohmann::json(now_iso8601_utc());
      } catch (...) {}
      // Ensure Playnite-managed games have a sensible graceful-exit timeout
      // Default to 10 seconds for the graceful-then-forceful shutdown recipe
      try {
        app["exit-timeout"] = 10;
      } catch (...) {}
      root["apps"].push_back(app);
      changed = true;
    }
  }

  void write_and_refresh_apps(nlohmann::json &root, const std::string &apps_path) {
    file_handler::write_file(apps_path.c_str(), root.dump(4));
    confighttp::refresh_client_apps_cache(root);
  }

  std::unordered_set<std::string> current_auto_ids(const nlohmann::json &root) {
    std::unordered_set<std::string> s;
    for (auto &a : root["apps"]) {
      try {
        if (a.contains("playnite-managed") && a["playnite-managed"].get<std::string>() == "auto") {
          s.insert(a.value("playnite-id", std::string()));
        }
      } catch (...) {}
    }
    return s;
  }

  std::size_t count_replacements_available(const std::unordered_set<std::string> &current_auto, const std::unordered_set<std::string> &selected_ids) {
    std::size_t c = 0;
    for (auto &id : selected_ids) {
      if (!current_auto.contains(id)) {
        ++c;
      }
    }
    return c;
  }

  void purge_uninstalled_and_ttl(nlohmann::json &root,
                                 const std::unordered_set<std::string> &uninstalled_lower,
                                 int delete_after_days,
                                 std::time_t now_time,
                                 const std::unordered_map<std::string, std::time_t> &last_played_map,
                                 bool recent_mode,
                                 bool require_repl,
                                 bool remove_uninstalled,
                                 const std::unordered_set<std::string> &selected_ids,
                                 bool &changed) {
    auto cur = current_auto_ids(root);
    auto repl = count_replacements_available(cur, selected_ids);
    nlohmann::json kept = nlohmann::json::array();
    for (auto &app : root["apps"]) {
      bool is_auto = false;
      std::string pid;
      try {
        is_auto = app.contains("playnite-managed") && app["playnite-managed"].get<std::string>() == "auto";
        if (app.contains("playnite-id")) {
          pid = app["playnite-id"].get<std::string>();
        }
      } catch (...) {}
      if (is_auto && !pid.empty()) {
        if ((remove_uninstalled && uninstalled_lower.contains(to_lower_copy(pid))) || should_ttl_delete(app, delete_after_days, now_time, last_played_map)) {
          changed = true;
          continue;
        }
        if (!selected_ids.contains(pid) && recent_mode && require_repl && repl > 0) {
          --repl;
          changed = true;
          continue;
        }
      }
      kept.push_back(app);
    }
    if (kept.size() != root["apps"].size()) {
      root["apps"] = std::move(kept);
    }
  }
}  // namespace platf::playnite::sync

namespace platf::playnite::sync {
  void autosync_reconcile(nlohmann::json &root,
                          const std::vector<Game> &all_games,
                          int recentN,
                          int recentAgeDays,
                          int delete_after_days,
                          bool require_repl,
                          bool sync_all_installed,
                          const std::vector<std::string> &categories,
                          const std::vector<std::string> &include_plugins,
                          const std::vector<std::string> &exclude_categories,
                          const std::vector<std::string> &exclude_ids,
                          const std::vector<std::string> &exclude_plugins,
                          bool remove_uninstalled,
                          bool &changed,
                          std::size_t &matched_out) {
    if (!root.contains("apps") || !root["apps"].is_array()) {
      root["apps"] = nlohmann::json::array();
    }
    changed = false;
    matched_out = 0;
    // Build installed and uninstalled sets
    std::vector<Game> installed;
    std::unordered_set<std::string> uninstalled_lower;
    snapshot_installed_and_uninstalled(all_games, installed, uninstalled_lower);

    // Build exclusion sets
    auto excl = build_exclusion_lower(exclude_ids);
    std::unordered_set<std::string> exclude_categories_lower;
    for (auto name : exclude_categories) {
      auto lowered = to_lower_copy(std::move(name));
      if (!lowered.empty()) {
        exclude_categories_lower.insert(std::move(lowered));
      }
    }
    std::unordered_set<std::string> exclude_plugins_lower;
    for (auto id : exclude_plugins) {
      auto lowered = to_lower_copy(std::move(id));
      if (!lowered.empty()) {
        exclude_plugins_lower.insert(std::move(lowered));
      }
    }

    // Select recent/category/plugin/all-installed sources and merge with flags
    std::unordered_map<std::string, int> source_flags;
    std::vector<Game> sel_recent, sel_cats, sel_plugins, sel_all;
    if (recentN > 0) {
      sel_recent = select_recent_installed_games(installed, recentN, recentAgeDays, excl, exclude_categories_lower, exclude_plugins_lower, source_flags);
    }
    if (!categories.empty()) {
      sel_cats = select_category_games(installed, categories, excl, exclude_categories_lower, exclude_plugins_lower, source_flags);
    }
    std::unordered_set<std::string> include_plugins_lower;
    for (auto id : include_plugins) {
      auto lowered = to_lower_copy(std::move(id));
      if (!lowered.empty()) {
        include_plugins_lower.insert(std::move(lowered));
      }
    }
    if (!include_plugins_lower.empty()) {
      sel_plugins = select_plugin_games(installed, include_plugins_lower, excl, exclude_categories_lower, exclude_plugins_lower, source_flags);
    }
    if (sync_all_installed) {
      sel_all = select_all_installed_games(installed, excl, exclude_categories_lower, exclude_plugins_lower, source_flags);
    }

    // Merge selections by id, preserving first instance
    std::unordered_map<std::string, const Game *> by_id;
    for (const auto &g : sel_recent) {
      if (!g.id.empty()) {
        by_id.emplace(g.id, &g);
      }
    }
    for (const auto &g : sel_cats) {
      if (!g.id.empty()) {
        if (!by_id.contains(g.id)) {
          by_id.emplace(g.id, &g);
        }
      }
    }
    for (const auto &g : sel_plugins) {
      if (!g.id.empty() && !by_id.contains(g.id)) {
        by_id.emplace(g.id, &g);
      }
    }
    for (const auto &g : sel_all) {
      if (!g.id.empty() && !by_id.contains(g.id)) {
        by_id.emplace(g.id, &g);
      }
    }
    std::vector<Game> selected;
    selected.reserve(by_id.size());
    for (auto &kv : by_id) {
      selected.push_back(*kv.second);
    }

    // Build indexes
    std::unordered_map<std::string, GameRef> by_exe, by_dir, by_id_idx;
    build_game_indexes(selected, by_exe, by_dir, by_id_idx);

    // Update existing
    std::unordered_set<std::string> matched_ids;
    iterate_existing_apps(root, by_id_idx, by_exe, by_dir, source_flags, matched_out, matched_ids, changed);

    // Purge
    auto last_played_map = build_last_played_map(installed);
    std::unordered_set<std::string> selected_ids;
    for (const auto &g : selected) {
      selected_ids.insert(g.id);
    }
    const bool recent_mode = (recentN > 0);
    purge_uninstalled_and_ttl(root, uninstalled_lower, delete_after_days, std::time(nullptr), last_played_map, recent_mode, require_repl, remove_uninstalled, selected_ids, changed);

    // Add missing
    add_missing_auto_entries(root, selected, matched_ids, source_flags, changed);
  }
}  // namespace platf::playnite::sync

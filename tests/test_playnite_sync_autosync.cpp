#include "src/platform/windows/playnite_sync.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

using namespace platf::playnite;
using namespace platf::playnite::sync;

static Game G(std::string id,
              std::string last,
              bool installed = true,
              std::vector<std::string> cats = {},
              std::string plugin = {}) {
  Game g;
  g.id = id;
  g.name = id;
  g.last_played = last;
  g.installed = installed;
  g.categories = cats;
  g.plugin_id = plugin;
  return g;
}

TEST(PlayniteSync_Recent, SortsByLastPlayedAndRespectsLimit) {
  auto n = now_iso8601_utc();
  // older than B, newer than C
  std::vector<Game> in {G("A", "2024-01-01T00:00:00Z"), G("B", "2025-01-01T00:00:00Z"), G("C", "2023-01-01T00:00:00Z")};
  std::unordered_set<std::string> excl;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto out = select_recent_installed_games(in, 2, 0, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].id, "B");
  EXPECT_EQ(out[1].id, "A");
  EXPECT_EQ(flags["B"] & 0x1, 0x1);
}

TEST(PlayniteSync_Recent, AgeFilterSkipsInvalidTimestamps) {
  // One invalid last_played, one valid recent
  std::vector<Game> in {G("A", "not-a-date"), G("B", now_iso8601_utc())};
  std::unordered_set<std::string> excl;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto out = select_recent_installed_games(in, 5, 30, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].id, "B");
}

TEST(PlayniteSync_Indexes, MatchAppByIdThenCmdThenDir) {
  std::vector<Game> sel {G("ID1", "2024-01-01T00:00:00Z"), G("ID2", "2024-01-01T00:00:00Z")};
  sel[1].exe = "C:/Games/Game.exe";
  sel[1].working_dir = "C:/Games";
  std::unordered_map<std::string, GameRef> by_exe, by_dir, by_id;
  build_game_indexes(sel, by_exe, by_dir, by_id);

  nlohmann::json app;
  // Prefer id
  app["playnite-id"] = "ID2";
  auto g = match_app_against_indexes(app, by_id, by_exe, by_dir);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->id, "ID2");

  // No id, match by cmd
  nlohmann::json app2;
  app2["cmd"] = "\"C:/Games/Game.exe\"";  // quotes and forward slashes acceptable
  g = match_app_against_indexes(app2, by_id, by_exe, by_dir);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->id, "ID2");

  // No id/cmd, match by working-dir
  nlohmann::json app3;
  app3["working-dir"] = "C:/Games";
  g = match_app_against_indexes(app3, by_id, by_exe, by_dir);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->id, "ID2");
}

TEST(PlayniteSync_Annotate, MarkAppFlagsSourceAndManaged) {
  nlohmann::json app = nlohmann::json::object();
  mark_app_as_playnite_auto(app, 0);
  EXPECT_EQ(app["playnite-source"], "unknown");
  EXPECT_EQ(app["playnite-managed"], "auto");
  mark_app_as_playnite_auto(app, 1);
  EXPECT_EQ(app["playnite-source"], "recent");
  mark_app_as_playnite_auto(app, 2);
  EXPECT_EQ(app["playnite-source"], "category");
  mark_app_as_playnite_auto(app, 3);
  EXPECT_EQ(app["playnite-source"], "recent+category");
}

TEST(PlayniteSync_TTL, NoDeleteWhenDisabledOrPlayedAfterAdded) {
  nlohmann::json app;
  app["playnite-id"] = "X";
  app["playnite-added-at"] = "2024-01-01T00:00:00Z";
  auto now = std::time(nullptr);
  std::unordered_map<std::string, std::time_t> last;
  // delete_after_days <= 0 disables TTL
  EXPECT_FALSE(should_ttl_delete(app, 0, now, last));
  // mark as played after added
  last["X"] = now;  // definitely >= added
  EXPECT_FALSE(should_ttl_delete(app, 1, now, last));
}

TEST(PlayniteSync_Purge, RemovesUninstalledOrExpiredOnly) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  // Two auto apps: A and B
  for (auto id : {"A", "B"}) {
    nlohmann::json a;
    a["playnite-id"] = id;
    a["playnite-managed"] = "auto";
    root["apps"].push_back(a);
  }
  std::unordered_set<std::string> uninstalled_lower {to_lower_copy(std::string("B"))};
  auto now = std::time(nullptr);
  std::unordered_map<std::string, std::time_t> last_played;
  bool changed = false;
  purge_uninstalled_and_ttl(root, uninstalled_lower, 0, now, last_played, changed);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");

  // Rebuild a simpler case: current_auto has X only; replacement candidate Y exists but X should stay unless uninstalled/expired
  nlohmann::json root2;
  root2["apps"] = nlohmann::json::array();
  nlohmann::json x;
  x["playnite-id"] = "X";
  x["playnite-managed"] = "auto";
  root2["apps"].push_back(x);
  std::unordered_set<std::string> none;
  changed = false;
  purge_uninstalled_and_ttl(root2, none, 0, now, last_played, changed);
  EXPECT_FALSE(changed);
  ASSERT_EQ(root2["apps"].size(), 1u);
  EXPECT_EQ(root2["apps"][0]["playnite-id"], "X");
}

TEST(PlayniteSync_AddMissing, AddsMissingSelectedWithMetadataAndTimestamps) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> selected {G("S1", "2024-01-01T00:00:00Z"), G("S2", "2024-01-02T00:00:00Z")};
  std::unordered_set<std::string> matched_ids {"S1"};
  std::unordered_map<std::string, int> src_flags;
  src_flags["S1"] = 1;
  src_flags["S2"] = 3;
  bool changed = false;
  add_missing_auto_entries(root, selected, matched_ids, src_flags, changed);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  const auto &app = root["apps"][0];
  EXPECT_EQ(app["playnite-id"], "S2");
  EXPECT_EQ(app["playnite-managed"], "auto");
  EXPECT_EQ(app["playnite-source"], "recent+category");
  EXPECT_TRUE(app.contains("playnite-added-at"));
}

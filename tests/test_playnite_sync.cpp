#include "src/platform/windows/playnite_sync.h"

#include <gtest/gtest.h>

using namespace platf::playnite;
using namespace platf::playnite::sync;

TEST(PlayniteSync_TimeParse, ParsesZuluAndOffset) {
  std::time_t t1 = 0, t2 = 0, t3 = 0;
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19T12:34:56Z", t1));
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19T14:34:56+02:00", t2));
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19 12:34:56", t3));
  EXPECT_EQ(t1, t2);  // +02 offset converts to same UTC
}

static Game make_game(std::string id,
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

TEST(PlayniteSync_Select, RecentSelectionHonorsAgeAndExclude) {
  // Two games, one recent, one old; exclude the recent by id
  auto now_iso = now_iso8601_utc();
  std::vector<Game> installed {
    make_game("A", now_iso, true),
    make_game("B", "2020-01-01T00:00:00Z", true)
  };
  std::unordered_set<std::string> excl {to_lower_copy(std::string("a"))};
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 1, 30, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 0u);  // recent candidate excluded; no fallback
}

TEST(PlayniteSync_Select, RecentSelectionSkipsExcludedCategories) {
  auto now_iso = now_iso8601_utc();
  std::vector<Game> installed {
    make_game("A", now_iso, true, {"Steam"}),
    make_game("B", now_iso, true, {"Indie"})
  };
  std::unordered_set<std::string> excl_ids;
  std::unordered_set<std::string> excl_categories {to_lower_copy(std::string("steam"))};
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 2, 0, excl_ids, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "B");
  EXPECT_EQ(flags["B"] & 0x1, 0x1);
}

TEST(PlayniteSync_Select, RecentSelectionSkipsExcludedPlugins) {
  auto now_iso = now_iso8601_utc();
  std::vector<Game> installed {
    make_game("A", now_iso, true, {}, "cb91dfc9-b977-43bf-8e70-55f46e410fab"),
    make_game("B", now_iso, true, {}, "83dd83a4-0cf7-49fb-9138-8547f6b60c18")
  };
  std::unordered_set<std::string> excl_ids;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins {to_lower_copy(std::string("cb91dfc9-b977-43bf-8e70-55f46e410fab"))};
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 2, 0, excl_ids, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "B");
}

TEST(PlayniteSync_Select, CategorySelectionMatchesAnyCategory) {
  std::vector<Game> installed {
    make_game("A", "2024-08-01T00:00:00Z", true, {"RPG", "Indie"}),
    make_game("B", "2024-08-01T00:00:00Z", true, {"Action"})
  };
  std::unordered_set<std::string> excl;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  std::vector<std::string> cats {"indie"};
  auto sel = select_category_games(installed, cats, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "A");
  EXPECT_EQ(flags["A"] & 0x2, 0x2);
}

TEST(PlayniteSync_Purge, TTLAndReplacementPolicy) {
  // Setup a minimal apps.json and selection
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  // Existing auto app older than TTL and never played
  nlohmann::json app;
  app["playnite-id"] = "X";
  app["playnite-managed"] = "auto";
  app["playnite-added-at"] = "2000-01-01T00:00:00Z";
  root["apps"].push_back(app);
  std::unordered_set<std::string> uninstalled;  // not uninstalled
  std::unordered_set<std::string> selected_ids;
  auto now = std::time(nullptr);
  std::unordered_map<std::string, std::time_t> last_played;  // empty => never played
  bool changed = false;
  purge_uninstalled_and_ttl(root, uninstalled, 1 /*days*/, now, last_played, true /*recent*/, true /*require repl*/, true /*remove uninstalled*/, false /*sync all*/, selected_ids, changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(root["apps"].size(), 0);
}

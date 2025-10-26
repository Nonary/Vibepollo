#include "src/platform/windows/playnite_sync.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

TEST(PlayniteAutosync_Reconcile, AddsSelectedGamesToEmptyApps) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true), G("B", "2024-01-01T00:00:00Z", true, {"RPG"})};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, /*recentN*/ 1, /*recentAgeDays*/ 0, /*delete_after_days*/ 0, /*require_repl*/ true,
                     /*categories*/ std::vector<std::string> {"RPG"},
                     /*exclude_categories*/ std::vector<std::string> {},
                     /*exclude*/ {},
                     /*exclude_plugins*/ std::vector<std::string> {},
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 2u);  // A from recent, B from category
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][1]["playnite-id"], "B");
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludeIds) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, {}, {}, {"a"}, /*exclude_plugins*/ {}, changed, matched);
  EXPECT_FALSE(changed);
  EXPECT_EQ(root["apps"].size(), 0u);
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludeCategories) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {
    G("A", "2025-01-01T00:00:00Z", true, {"Steam"}),
    G("B", "2024-12-01T00:00:00Z", true, {"Indie"})
  };
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all,
                     /*recentN*/ 2,
                     /*recentAgeDays*/ 0,
                     /*delete_after_days*/ 0,
                     /*require_repl*/ true,
                     /*categories*/ std::vector<std::string> {},
                     /*exclude_categories*/ std::vector<std::string> {"Steam"},
                     /*exclude_ids*/ {},
                     /*exclude_plugins*/ std::vector<std::string> {},
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "B");
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludePlugins) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {
    G("A", "2025-01-01T00:00:00Z", true, {}, "CB91DFC9-B977-43BF-8E70-55F46E410FAB"),
    G("B", "2025-01-02T00:00:00Z", true, {}, "83DD83A4-0CF7-49FB-9138-8547F6B60C18")
  };
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root,
                     all,
                     /*recentN*/ 2,
                     /*recentAgeDays*/ 0,
                     /*delete_after_days*/ 0,
                     /*require_repl*/ true,
                     /*categories*/ std::vector<std::string> {},
                     /*exclude_categories*/ std::vector<std::string> {},
                     /*exclude_ids*/ {},
                     /*exclude_plugins*/ std::vector<std::string> {"cb91dfc9-b977-43bf-8e70-55f46e410fab"},
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "B");
}

TEST(PlayniteAutosync_Reconcile, UpdatesExistingAndSetsManagedFields) {
  // Existing app matching by id gets annotated and not duplicated
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json a;
  a["playnite-id"] = "A";
  root["apps"].push_back(a);
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, {}, {}, {}, {}, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][0]["playnite-managed"], "auto");
}

TEST(PlayniteAutosync_Reconcile, PreservesExistingOrderAndAppendsNew) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json manual;
  manual["name"] = "Manual Entry";
  root["apps"].push_back(manual);
  nlohmann::json autoA;
  autoA["playnite-id"] = "A";
  autoA["playnite-managed"] = "auto";
  root["apps"].push_back(autoA);

  std::vector<Game> all {G("A", "2025-01-05T00:00:00Z", true), G("D", "2025-01-02T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 2, 0, 0, true, {}, {}, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 3u);
  EXPECT_FALSE(root["apps"][0].contains("playnite-id"));
  EXPECT_EQ(root["apps"][1]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][2]["playnite-id"], "D");
}

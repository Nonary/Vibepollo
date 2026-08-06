#include "src/config_playnite.h"
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

TEST(PlayniteConfig, Booleans_ParseCaseInsensitiveTruths) {
  std::unordered_map<std::string, std::string> vars {
    {"playnite_auto_sync", "on"},
    {"playnite_autosync_require_replacement", "0"},
    {"playnite_sync_all_installed", "YES"},
    {"playnite_autosync_remove_uninstalled", "off"}
  };
  const auto parsed = config::parse_playnite(vars);
  EXPECT_TRUE(parsed.auto_sync);
  EXPECT_FALSE(parsed.autosync_require_replacement);
  EXPECT_TRUE(parsed.sync_all_installed);
  EXPECT_FALSE(parsed.autosync_remove_uninstalled);
  EXPECT_TRUE(vars.empty());
}

TEST(PlayniteConfig, Integers_ValidAndClampNegatives) {
  std::unordered_map<std::string, std::string> vars {{"playnite_recent_games", "20"}, {"playnite_recent_max_age_days", "-5"}, {"playnite_autosync_delete_after_days", "7"}, {"playnite_focus_attempts", "0"}, {"playnite_focus_timeout_secs", "12"}};
  const auto parsed = config::parse_playnite(vars);
  EXPECT_EQ(parsed.recent_games, 20);
  EXPECT_EQ(parsed.recent_max_age_days, 0);  // clamped
  EXPECT_EQ(parsed.autosync_delete_after_days, 7);
  EXPECT_EQ(parsed.focus_attempts, 0);
  EXPECT_EQ(parsed.focus_timeout_secs, 12);
}

TEST(PlayniteConfig, Integers_InvalidStringsAreIgnored) {
  // Leave defaults when invalid
  std::unordered_map<std::string, std::string> vars {{"playnite_recent_games", "abc"}, {"playnite_focus_attempts", "-x"}, {"playnite_focus_timeout_secs", ""}};
  const auto parsed = config::parse_playnite(vars);
  EXPECT_EQ(parsed.recent_games, 10);
  EXPECT_EQ(parsed.focus_attempts, 3);
  EXPECT_EQ(parsed.focus_timeout_secs, 15);
}

TEST(PlayniteConfig, Lists_ParseJsonArrayAndCsv) {
  std::unordered_map<std::string, std::string> vars {
    {"playnite_sync_categories", "[\"A\",\"B\"]"},
    {"playnite_exclude_categories", "[{\"id\":\"deck\",\"name\":\"Steam Deck\"},\"Indie\"]"},
    {"playnite_sync_plugins", "[{\"id\":\"steam\",\"name\":\"Steam\"},\"gog\"]"},
    {"playnite_exclude_plugins", "[{\"id\":\"steam\",\"name\":\"Steam\"},\"gog\"]"},
    {"playnite_exclude_games", " x , y, z "}
  };
  const auto parsed = config::parse_playnite(vars);
  ASSERT_EQ(parsed.sync_categories.size(), 2u);
  EXPECT_EQ(parsed.sync_categories[0], "A");
  ASSERT_EQ(parsed.exclude_categories.size(), 2u);
  EXPECT_EQ(parsed.exclude_categories[0], "Steam Deck");
  EXPECT_EQ(parsed.exclude_categories[1], "Indie");
  ASSERT_EQ(parsed.exclude_plugins.size(), 2u);
  EXPECT_EQ(parsed.exclude_plugins[0], "steam");
  EXPECT_EQ(parsed.exclude_plugins[1], "gog");
  ASSERT_EQ(parsed.exclude_plugins_meta.size(), 2u);
  EXPECT_EQ(parsed.exclude_plugins_meta[0].name, "Steam");
  ASSERT_EQ(parsed.sync_plugins.size(), 2u);
  EXPECT_EQ(parsed.sync_plugins[0], "steam");
  EXPECT_EQ(parsed.sync_plugins[1], "gog");
  ASSERT_EQ(parsed.sync_plugins_meta.size(), 2u);
  EXPECT_EQ(parsed.sync_plugins_meta[0].name, "Steam");
  ASSERT_EQ(parsed.exclude_games.size(), 3u);
  EXPECT_EQ(parsed.exclude_games[0], "x");
  EXPECT_EQ(parsed.exclude_games[2], "z");
}

// Note: extensions-dir override removed; no path config test required here.

TEST(PlayniteConfig, FocusExitOnFirst_ParsesBoolean) {
  std::unordered_map<std::string, std::string> vars {{"playnite_focus_exit_on_first", "true"}};
  const auto parsed = config::parse_playnite(vars);
  EXPECT_TRUE(parsed.focus_exit_on_first);
}

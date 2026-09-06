#include "src/config_steam.h"

#include <gtest/gtest.h>

TEST(SteamConfig, DefaultsToOptInRecentSynchronizationOnBothPlatforms) {
  std::unordered_map<std::string, std::string> vars;
  const auto parsed = config::parse_steam(vars);
  for (const bool linux_host : {false, true}) {
    const auto result = config::normalize_steam_policy(parsed, linux_host);
    EXPECT_FALSE(result.auto_sync);
    EXPECT_FALSE(result.sync_all_installed);
    EXPECT_EQ(result.recent_games, 10);
    EXPECT_EQ(result.recent_max_age_days, 30);
  }
}

TEST(SteamConfig, PreservesExplicitSyncOptIns) {
  std::unordered_map<std::string, std::string> vars {
    {"steam_auto_sync", "on"},
    {"steam_sync_all_installed", "on"},
  };
  const auto result = config::parse_steam(vars);
  EXPECT_TRUE(result.auto_sync);
  EXPECT_TRUE(result.sync_all_installed);
  EXPECT_TRUE(vars.empty());
}

TEST(SteamConfig, ParsesIndependentFlags) {
  std::unordered_map<std::string, std::string> vars {{"steam_enabled", "off"}, {"steam_auto_sync", "off"}};
  const auto result = config::parse_steam(vars);
#if defined(__linux__)
  EXPECT_TRUE(result.enabled);
  EXPECT_FALSE(result.auto_sync);
#else
  EXPECT_FALSE(result.enabled);
  EXPECT_FALSE(result.auto_sync);
#endif
  EXPECT_TRUE(vars.empty());
}

TEST(SteamConfig, LinuxPolicyForcesSteamOn) {
  config::steam_t value;
  value.enabled = false;
  value.auto_sync = false;
  const auto result = config::normalize_steam_policy(value, true);
  EXPECT_TRUE(result.enabled);
  EXPECT_FALSE(result.auto_sync);
}

TEST(SteamConfig, ParsesExclusionsAndRemovalPolicy) {
  std::unordered_map<std::string, std::string> vars {
    {"steam_exclude_games", R"(["570",{"id":"730","name":"Counter-Strike 2"},{"name":"Legacy Name"}])"},
    {"steam_autosync_remove_uninstalled", "off"},
    {"steam_include_tools", "on"},
  };
  const auto result = config::parse_steam(vars);
  ASSERT_EQ(result.exclude_games.size(), 3U);
  EXPECT_EQ(result.exclude_games[0], "570");
  EXPECT_EQ(result.exclude_games_meta[1].id, "730");
  EXPECT_EQ(result.exclude_games_meta[2].name, "Legacy Name");
  EXPECT_FALSE(result.autosync_remove_uninstalled);
  EXPECT_TRUE(result.include_tools);
  EXPECT_TRUE(vars.empty());
}

TEST(SteamConfig, ParsesRecentSynchronizationPolicy) {
  std::unordered_map<std::string, std::string> vars {
    {"steam_sync_all_installed", "off"},
    {"steam_recent_games", "15"},
    {"steam_recent_max_age_days", "45"},
  };
  const auto result = config::parse_steam(vars);
  EXPECT_FALSE(result.sync_all_installed);
  EXPECT_EQ(result.recent_games, 15);
  EXPECT_EQ(result.recent_max_age_days, 45);
  EXPECT_TRUE(vars.empty());
}

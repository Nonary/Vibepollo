#include "src/config_lutris.h"

#include <gtest/gtest.h>

TEST(LutrisConfig, ParsesFlagsAndExclusions) {
  std::unordered_map<std::string, std::string> vars {
    {"lutris_enabled", "off"}, {"lutris_auto_sync", "off"},
    {"lutris_autosync_remove_uninstalled", "off"}, {"lutris_include_steam", "on"},
    {"lutris_exclude_games", R"([{"id":"7","name":"Example"},{"name":"Legacy"}])"},
  };
  const auto result = config::parse_lutris(vars);
#if defined(__linux__)
  EXPECT_FALSE(result.enabled);
#else
  EXPECT_FALSE(result.enabled);
#endif
  EXPECT_FALSE(result.auto_sync);
  EXPECT_FALSE(result.autosync_remove_uninstalled);
  EXPECT_TRUE(result.include_steam);
  ASSERT_EQ(result.exclude_games_meta.size(), 2U);
  EXPECT_EQ(result.exclude_games_meta[0].id, "7");
  EXPECT_EQ(result.exclude_games_meta[1].name, "Legacy");
  EXPECT_TRUE(vars.empty());
}

TEST(LutrisConfig, NonLinuxPolicyDisablesProvider) {
  config::lutris_t value;
  value.enabled = true;
  EXPECT_FALSE(config::normalize_lutris_policy(value, false).enabled);
  EXPECT_TRUE(config::normalize_lutris_policy(value, true).enabled);
}

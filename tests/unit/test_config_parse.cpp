/**
 * @file tests/unit/test_config_parse.cpp
 * @brief Tests for Sunshine config file parsing.
 */
#include "../tests_common.h"

#include <src/config.h>

#include <string>

TEST(ConfigParse, NormalizesUtf8BomPrefixedKeys) {
  const auto vars = config::parse_config(
    std::string("\xEF\xBB\xBF") + "virtual_display_mode = per_client\n"
  );

  ASSERT_TRUE(vars.contains("virtual_display_mode"));
  EXPECT_EQ(vars.at("virtual_display_mode"), "per_client");
}

TEST(ConfigParse, NormalizesMojibakeBomPrefixedKeys) {
  const auto vars = config::parse_config(
    std::string("\xC3\xAF\xC2\xBB\xC2\xBF") + "virtual_display_mode = per_client\n"
  );

  ASSERT_TRUE(vars.contains("virtual_display_mode"));
  EXPECT_EQ(vars.at("virtual_display_mode"), "per_client");
}

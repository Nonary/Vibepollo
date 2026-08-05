/**
 * @file tests/unit/test_config_parse.cpp
 * @brief Tests for Sunshine config file parsing.
 */
#include "../tests_common.h"

#include <src/config_key.h>

#include <string>

TEST(ConfigParse, NormalizesUtf8BomPrefixedKeys) {
  EXPECT_EQ(config::normalize_config_key(std::string("\xEF\xBB\xBF") + "virtual_display_mode"), "virtual_display_mode");
}

TEST(ConfigParse, NormalizesMojibakeBomPrefixedKeys) {
  EXPECT_EQ(config::normalize_config_key(std::string("\xC3\xAF\xC2\xBB\xC2\xBF") + "virtual_display_mode"), "virtual_display_mode");
}

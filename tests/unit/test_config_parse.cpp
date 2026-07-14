/**
 * @file tests/unit/test_config_parse.cpp
 * @brief Tests for Sunshine config file parsing.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/utility.h>

#include <filesystem>
#include <fstream>
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

TEST(ConfigParse, AmdHotApplyDoesNotReloadUnrelatedConfig) {
  const auto original_video = config::video;
  const auto original_config_file = config::sunshine.config_file;
  const auto original_runtime_overrides = config::runtime_config_overrides_snapshot();
  const auto test_config = std::filesystem::temp_directory_path() / "vibepollo-amd-hot-apply-test.conf";
  auto restore_config = util::fail_guard([&]() {
    config::video = original_video;
    config::sunshine.config_file = original_config_file;
    config::set_runtime_config_overrides(original_runtime_overrides);
    std::error_code ec;
    std::filesystem::remove(test_config, ec);
  });

  config::clear_runtime_config_overrides();
  config::sunshine.config_file = test_config.string();
  config::video.qp = 37;
  {
    std::ofstream output(test_config, std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << "qp = 4\n"
           << "amd_quality = speed\n"
           << "amd_input_queue_size = 7\n"
           << "amd_smart_access_video = enabled\n"
           << "amd_av1_latency_mode = lowest\n";
  }

  config::apply_amd_config_now();

  EXPECT_EQ(config::video.qp, 37);
  EXPECT_EQ(config::video.amd.amd_input_queue_size, 7);
  ASSERT_TRUE(config::video.amd.amd_quality_av1.has_value());
  EXPECT_EQ(*config::video.amd.amd_quality_av1, 100);
  ASSERT_TRUE(config::video.amd.amd_smart_access_video.has_value());
  EXPECT_EQ(*config::video.amd.amd_smart_access_video, 1);
  ASSERT_TRUE(config::video.amd.amd_av1_latency_mode.has_value());
  EXPECT_EQ(*config::video.amd.amd_av1_latency_mode, 3);
}

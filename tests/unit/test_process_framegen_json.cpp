/**
 * @file tests/unit/test_process_framegen_json.cpp
 * @brief Deterministic per-app frame-generation JSON parsing contracts.
 */
#include "../tests_common.h"

#include <src/app_framegen_config.h>

namespace {
  TEST(ProcessFramegenJson, LegacyAndCurrentCaptureFixKeysStillParse) {
    const auto parsed = proc::app_config::parse_framegen_json(R"json({
      "dlss-framegen-capture-fix": true,
      "gen2-framegen-fix": false
    })json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->gen1_capture_fix);
    EXPECT_FALSE(parsed->gen2_capture_fix);
    EXPECT_TRUE(parsed->enabled);
  }

  TEST(ProcessFramegenJson, CurrentGen1CaptureFixKeyParses) {
    const auto parsed = proc::app_config::parse_framegen_json(R"json({"gen1-framegen-fix":true})json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->gen1_capture_fix);
    EXPECT_TRUE(parsed->enabled);
  }

  TEST(ProcessFramegenJson, FrameGenerationModeOffOverridesStaleProvider) {
    const auto parsed = proc::app_config::parse_framegen_json(R"json({
      "frame-generation-mode": "off",
      "frame-generation-provider": "nvidia-smooth-motion",
      "lossless-scaling-framegen": true
    })json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->enabled);
    EXPECT_FALSE(parsed->lossless_scaling_framegen);
    EXPECT_EQ(parsed->provider, "lossless-scaling");
  }

  TEST(ProcessFramegenJson, FrameGenerationModeSelectsProvider) {
    const auto parsed = proc::app_config::parse_framegen_json(R"json({
      "frame-generation-mode": "nvidia-smooth-motion"
    })json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->enabled);
    EXPECT_FALSE(parsed->lossless_scaling_framegen);
    EXPECT_EQ(parsed->provider, "nvidia-smooth-motion");
  }

  TEST(ProcessFramegenJson, MalformedInputIsRejectedWithoutState) {
    EXPECT_FALSE(proc::app_config::parse_framegen_json("{broken").has_value());
    EXPECT_FALSE(proc::app_config::parse_framegen_json("[]").has_value());
  }
}  // namespace

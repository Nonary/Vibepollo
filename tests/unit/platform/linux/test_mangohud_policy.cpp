/**
 * @file tests/unit/platform/linux/test_mangohud_policy.cpp
 * @brief Tests for Linux MangoHUD frame-limiter launch policy.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/mangohud_policy.h>

namespace mangohud = platf::mangohud;

TEST(MangoHudPolicy, SelectsMangoHudForLinuxAutoAndExplicitProviders) {
  EXPECT_TRUE(mangohud::provider_selected("auto"));
  EXPECT_TRUE(mangohud::provider_selected("MangoHUD"));
  EXPECT_TRUE(mangohud::provider_selected("mango-hud"));
  EXPECT_FALSE(mangohud::provider_selected("none"));
  EXPECT_FALSE(mangohud::provider_selected("rtss"));
  EXPECT_FALSE(mangohud::provider_selected("nvidia-control-panel"));
}

TEST(MangoHudPolicy, UsesStreamRateAndPreservesFractionalLimits) {
  framegen::stream_start_policy_t stream_policy;
  stream_policy.fps = 60;
  auto policy = mangohud::make_launch_policy("auto", true, false, stream_policy, 0);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit_millihz, 60000u);
  EXPECT_EQ(policy.limit, "60");

  stream_policy.frame_limit_millihz = 59940;
  policy = mangohud::make_launch_policy("mangohud", true, false, stream_policy, 0);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "59.94");

  policy = mangohud::make_launch_policy("mangohud", true, false, stream_policy, 117500);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "117.5");
}

TEST(MangoHudPolicy, AddsAndRemovesOnlyItsOpenGlPreload) {
  const std::string existing = "/opt/lib/first.so:/opt/lib/second.so";
  const auto enabled = mangohud::with_preload(existing);
  EXPECT_EQ(
    enabled,
    existing + ":" + std::string(mangohud::preload_library)
  );
  EXPECT_EQ(mangohud::with_preload(enabled), enabled);
  EXPECT_EQ(mangohud::without_preload(enabled), existing);
  EXPECT_EQ(mangohud::without_preload(existing), existing);
}

TEST(MangoHudPolicy, AutomaticVirtualLimiterDoesNotEnablePhysicalStreams) {
  framegen::stream_start_policy_t stream_policy;
  stream_policy.fps = 120;

  EXPECT_FALSE(mangohud::make_launch_policy("auto", false, true, stream_policy, 0).enabled);
  stream_policy.uses_virtual_display = true;
  const auto policy = mangohud::make_launch_policy("auto", false, true, stream_policy, 0);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "120");
}

TEST(MangoHudPolicy, HonorsLosslessLimitBeforeGlobalOverride) {
  framegen::stream_start_policy_t stream_policy;
  stream_policy.fps = 120;
  stream_policy.lossless_rtss_limit = 60;

  auto policy = mangohud::make_launch_policy("auto", true, false, stream_policy, 0);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "60");

  policy = mangohud::make_launch_policy("auto", true, false, stream_policy, 59940);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "59.94");
}

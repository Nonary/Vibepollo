/**
 * @file tests/unit/platform/linux/test_mangohud_policy.cpp
 * @brief Tests for Linux MangoHUD frame-limiter launch policy.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/mangohud_policy.h>
#include <src/platform/linux/mangohud_state.h>

#include <chrono>

namespace mangohud = platf::mangohud;

TEST(MangoHudPolicy, SelectsOnlyExplicitMangoHudProvider) {
  EXPECT_FALSE(mangohud::provider_selected("auto"));
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

TEST(MangoHudPolicy, BuildsSteamCompatibleAuthoritativeLimitConfig) {
  EXPECT_EQ(mangohud::config_override("120"), "read_cfg,fps_limit_method=late,fps_limit=120");
  EXPECT_EQ(
    mangohud::config_override("59.94", "custom", false, "early"),
    "read_cfg,fps_limit_method=early,fps_limit=59.94"
  );
  EXPECT_EQ(
    mangohud::config_override("120", "3", false),
    "read_cfg,preset=3,no_display=0,fps_limit_method=late,fps_limit=120"
  );
  EXPECT_EQ(
    mangohud::config_override("120", "custom", true),
    "read_cfg,no_display=0,frame_timing=1,fps_limit_method=late,fps_limit=120"
  );
  EXPECT_EQ(
    mangohud::config_override("120", "4", true),
    "read_cfg,preset=4,no_display=0,frame_timing=1,fps_limit_method=late,fps_limit=120"
  );
  EXPECT_EQ(
    mangohud::config_override("position=top-right,fps_limit=30", "120", "custom", false),
    "read_cfg,position=top-right,fps_limit=30,fps_limit_method=late,fps_limit=120"
  );
  EXPECT_EQ(
    mangohud::overlay_config_override("position=top-right,fps_limit=30", "custom", false),
    "read_cfg,position=top-right,fps_limit=30,no_display=0,fps_limit=0"
  );
  EXPECT_EQ(
    mangohud::overlay_config_override({}, "3", true),
    "read_cfg,preset=3,no_display=0,frame_timing=1,fps_limit=0"
  );
}

TEST(MangoHudPolicy, AvoidsMangoHudFractionalEnvironmentTruncation) {
  EXPECT_EQ(mangohud::fps_limit_environment_override(120000, "120"), "120");
  EXPECT_TRUE(mangohud::fps_limit_environment_override(59940, "59.94").empty());
}

TEST(MangoHudPolicy, ValidatesAndSerializesLastMileSteamState) {
  EXPECT_TRUE(mangohud::valid_steam_app_id("480"));
  EXPECT_FALSE(mangohud::valid_steam_app_id("0"));
  EXPECT_FALSE(mangohud::valid_steam_app_id("48/0"));

  const auto state = mangohud::serialize_state(
    "proton",
    "59.94",
    "3",
    true,
    "early",
    std::chrono::system_clock::time_point {std::chrono::seconds {12345}}
  );
  EXPECT_NE(
    state.find("version=2\nprovider=proton\nlimit=59.94\npreset=3\nalways_show_graph=1\nlimiter_method=early\n"),
    std::string::npos
  );
  EXPECT_NE(state.find("owner_pid="), std::string::npos);
  EXPECT_TRUE(state.ends_with("expires=12345\n"));
}

TEST(MangoHudPolicy, SelectsProtonAsLinuxLimiterProvider) {
  framegen::stream_start_policy_t stream_policy;
  stream_policy.fps = 116;
  EXPECT_TRUE(mangohud::proton_provider_selected("Proton"));
  EXPECT_TRUE(mangohud::proton_provider_selected("MangoHUD + Proton"));
  EXPECT_TRUE(mangohud::proton_provider_selected("auto"));
  EXPECT_TRUE(mangohud::proton_overlay_provider_selected("auto"));
  EXPECT_TRUE(mangohud::proton_overlay_provider_selected("mangohud-proton"));
  EXPECT_FALSE(mangohud::proton_overlay_provider_selected("proton"));
  EXPECT_FALSE(mangohud::provider_selected("proton"));
  const auto policy = mangohud::make_launch_policy("proton", true, false, stream_policy, 0);
  ASSERT_TRUE(policy.enabled);
  EXPECT_EQ(policy.limit, "116");

  const auto overlay_policy = mangohud::make_launch_policy(
    "mangohud-proton", true, false, stream_policy, 0
  );
  ASSERT_TRUE(overlay_policy.enabled);
  EXPECT_EQ(overlay_policy.limit, "116");
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

/**
 * @file tests/unit/test_framegen_policy.cpp
 */
#include "../tests_common.h"

#include <src/framegen_policy.h>

#include <string>

namespace {

  framegen::stream_start_policy_t make_policy(
    std::string provider,
    bool uses_virtual_display,
    std::string capture_mode,
    bool lossless_scaling_framegen = false,
    bool auto_virtual_framegen_limiter = true
  ) {
    return framegen::make_stream_start_policy({
      .fps = 60,
      .frame_generation_enabled = provider != "lossless-scaling" || lossless_scaling_framegen,
      .gen1_framegen_fix = false,
      .gen2_framegen_fix = false,
      .lossless_scaling_framegen = lossless_scaling_framegen,
      .lossless_rtss_limit = std::nullopt,
      .frame_generation_provider = std::move(provider),
      .uses_virtual_display = uses_virtual_display,
      .capture_mode = std::move(capture_mode),
      .auto_capture_uses_wgc = true,
      .auto_virtual_framegen_limiter = auto_virtual_framegen_limiter,
    });
  }

  framegen::stream_start_policy_t make_policy_with_legacy_capture_fix(
    bool frame_generation_enabled,
    bool uses_virtual_display
  ) {
    return framegen::make_stream_start_policy({
      .fps = 60,
      .frame_generation_enabled = frame_generation_enabled,
      .gen1_framegen_fix = true,
      .gen2_framegen_fix = true,
      .lossless_scaling_framegen = false,
      .lossless_rtss_limit = std::nullopt,
      .frame_generation_provider = "lossless-scaling",
      .uses_virtual_display = uses_virtual_display,
      .capture_mode = "",
      .auto_capture_uses_wgc = true,
      .auto_virtual_framegen_limiter = true,
    });
  }

  TEST(FramegenPolicy, VirtualAutoWgcFrameGenerationUsesFourTimesRefresh) {
    for (const auto &provider : {"game-provided", "lossless-scaling", "nvidia-smooth-motion"}) {
      const bool lossless = std::string {provider} == "lossless-scaling";
      const auto policy = make_policy(provider, true, "", lossless);

      EXPECT_TRUE(policy.frame_generation_enabled);
      EXPECT_TRUE(policy.uses_virtual_display);
      EXPECT_TRUE(policy.effective_wgc_capture);
      EXPECT_FALSE(policy.physical_framegen_capture);
      ASSERT_TRUE(policy.framegen_refresh_rate.has_value());
      EXPECT_EQ(*policy.framegen_refresh_rate, 240);
      EXPECT_EQ(policy.refresh_multiplier, 4);
      EXPECT_TRUE(policy.auto_virtual_framegen_limiter);
    }
  }

  TEST(FramegenPolicy, VirtualExplicitWgcFrameGenerationUsesFourTimesRefresh) {
    const auto policy = make_policy("game-provided", true, "wgc");

    EXPECT_TRUE(policy.effective_wgc_capture);
    ASSERT_TRUE(policy.framegen_refresh_rate.has_value());
    EXPECT_EQ(*policy.framegen_refresh_rate, 240);
  }

  TEST(FramegenPolicy, PhysicalFrameGenerationDoesNotSetFramegenRefreshRate) {
    for (const auto &provider : {"game-provided", "lossless-scaling", "nvidia-smooth-motion"}) {
      const bool lossless = std::string {provider} == "lossless-scaling";
      const auto policy = make_policy(provider, false, "", lossless);

      EXPECT_TRUE(policy.frame_generation_enabled);
      EXPECT_FALSE(policy.requires_virtual_display);
      EXPECT_FALSE(policy.uses_virtual_display);
      EXPECT_FALSE(policy.effective_wgc_capture);
      EXPECT_TRUE(policy.physical_framegen_capture);
      EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
      EXPECT_EQ(policy.refresh_multiplier, 1);
    }
  }

  TEST(FramegenPolicy, ExplicitDxgiDoesNotEnterVirtualWgcPolicy) {
    const auto policy = make_policy("game-provided", true, "ddx");

    EXPECT_TRUE(policy.frame_generation_enabled);
    EXPECT_TRUE(policy.uses_virtual_display);
    EXPECT_FALSE(policy.effective_wgc_capture);
    EXPECT_FALSE(policy.physical_framegen_capture);
    EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
    EXPECT_EQ(policy.refresh_multiplier, 1);
  }

  TEST(FramegenPolicy, VirtualAutoLimiterCanBeOptedOutWithoutChangingRefreshPolicy) {
    const auto policy = make_policy("nvidia-smooth-motion", true, "", false, false);

    EXPECT_TRUE(policy.frame_generation_enabled);
    EXPECT_TRUE(policy.effective_wgc_capture);
    ASSERT_TRUE(policy.framegen_refresh_rate.has_value());
    EXPECT_EQ(*policy.framegen_refresh_rate, 240);
    EXPECT_EQ(policy.refresh_multiplier, 4);
    EXPECT_FALSE(policy.auto_virtual_framegen_limiter);
  }

  TEST(FramegenPolicy, LosslessProviderAloneDoesNotEnableFrameGeneration) {
    const auto policy = make_policy("lossless-scaling", true, "", false);

    EXPECT_FALSE(policy.frame_generation_enabled);
    EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
    EXPECT_EQ(policy.refresh_multiplier, 1);
  }

  TEST(FramegenPolicy, PlainVirtualDisplayAutoEnablesLimiterWithoutFrameGeneration) {
    // A virtual screen with no frame generation still auto-enables the limiter (Reflex on
    // NVIDIA) because virtual displays always run at a multiplied refresh. The 4x refresh
    // itself is enforced at virtual-display creation, not in the policy, so the policy-level
    // multiplier/refresh stay neutral here.
    const auto policy = make_policy("lossless-scaling", true, "", false);

    EXPECT_FALSE(policy.frame_generation_enabled);
    EXPECT_TRUE(policy.uses_virtual_display);
    EXPECT_TRUE(policy.auto_virtual_framegen_limiter);
    EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
    EXPECT_EQ(policy.refresh_multiplier, 1);
  }

  TEST(FramegenPolicy, PlainVirtualDisplayLimiterRespectsOptOut) {
    const auto policy = make_policy("lossless-scaling", true, "", false, false);

    EXPECT_FALSE(policy.frame_generation_enabled);
    EXPECT_TRUE(policy.uses_virtual_display);
    EXPECT_FALSE(policy.auto_virtual_framegen_limiter);
  }

  TEST(FramegenPolicy, PhysicalDisplayWithoutFrameGenerationDoesNotAutoEnableLimiter) {
    const auto policy = make_policy("lossless-scaling", false, "", false);

    EXPECT_FALSE(policy.uses_virtual_display);
    EXPECT_FALSE(policy.auto_virtual_framegen_limiter);
  }

  TEST(FramegenPolicy, LegacyCaptureFixFlagsDoNotEnableFrameGeneration) {
    const auto policy = make_policy_with_legacy_capture_fix(false, false);

    EXPECT_FALSE(policy.capture_fix_enabled);
    EXPECT_FALSE(policy.frame_generation_enabled);
    EXPECT_FALSE(policy.requires_virtual_display);
    EXPECT_FALSE(policy.physical_framegen_capture);
    EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
  }

  TEST(FramegenPolicy, LegacyCaptureFixFlagsDoNotForceVirtualDisplay) {
    const auto policy = make_policy_with_legacy_capture_fix(true, false);

    EXPECT_FALSE(policy.capture_fix_enabled);
    EXPECT_TRUE(policy.frame_generation_enabled);
    EXPECT_FALSE(policy.requires_virtual_display);
    EXPECT_TRUE(policy.physical_framegen_capture);
    EXPECT_FALSE(policy.framegen_refresh_rate.has_value());
  }

}  // namespace

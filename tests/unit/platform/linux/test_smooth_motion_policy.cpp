/**
 * @file tests/unit/platform/linux/test_smooth_motion_policy.cpp
 * @brief Tests for Linux NVIDIA Smooth Motion launch policy.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/smooth_motion_policy.h>

namespace smooth_motion = platf::smooth_motion;

TEST(SmoothMotionPolicy, EnablesOnlyTheSelectedNvidiaProvider) {
  EXPECT_TRUE(smooth_motion::make_launch_policy(true, "nvidia-smooth-motion", false).enabled);
  EXPECT_TRUE(smooth_motion::make_launch_policy(true, "NVIDIA Smooth Motion", false).enabled);
  EXPECT_FALSE(smooth_motion::make_launch_policy(false, "nvidia-smooth-motion", false).enabled);
  EXPECT_FALSE(smooth_motion::make_launch_policy(true, "game-provided", false).enabled);
  EXPECT_FALSE(smooth_motion::make_launch_policy(true, "lossless-scaling", false).enabled);
}

TEST(SmoothMotionPolicy, UsesGraphicsQueueOnlyForMangoHudCompatibility) {
  auto policy = smooth_motion::make_launch_policy(true, "nvidia-smooth-motion", false);
  EXPECT_FALSE(policy.use_graphics_queue);

  policy = smooth_motion::make_launch_policy(true, "nvidia-smooth-motion", true);
  EXPECT_TRUE(policy.use_graphics_queue);

  policy = smooth_motion::make_launch_policy(true, "game-provided", true);
  EXPECT_FALSE(policy.use_graphics_queue);
}

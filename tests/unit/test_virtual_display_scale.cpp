/**
 * @file tests/unit/test_virtual_display_scale.cpp
 * @brief Cross-platform virtual-display scaling policy tests.
 */
#include "../tests_common.h"

#include <src/virtual_display_scale.h>

TEST(VirtualDisplayScale, RecommendationTracksShortResolutionEdge) {
  EXPECT_EQ(virtual_display_scale::effective_percent(-1, 1920, 1080), 125u);
  EXPECT_EQ(virtual_display_scale::effective_percent(-1, 2560, 1440), 175u);
  EXPECT_EQ(virtual_display_scale::effective_percent(-1, 3024, 1890), 225u);
  EXPECT_EQ(virtual_display_scale::effective_percent(-1, 3840, 2160), 250u);
  EXPECT_EQ(virtual_display_scale::effective_percent(-1, 1440, 2560), 175u);
}

TEST(VirtualDisplayScale, PreserveUsesRetainedCompositorChoice) {
  EXPECT_DOUBLE_EQ(virtual_display_scale::effective_factor(0, 3840, 2160, 1.0, 1.75), 1.75);
  EXPECT_DOUBLE_EQ(virtual_display_scale::effective_factor(0, 3840, 2160, 1.25), 1.25);
}

TEST(VirtualDisplayScale, ExactAndRecommendedIgnoreRetainedChoice) {
  EXPECT_DOUBLE_EQ(virtual_display_scale::effective_factor(200, 1920, 1080, 1.0, 1.75), 2.0);
  EXPECT_DOUBLE_EQ(virtual_display_scale::effective_factor(-1, 2560, 1440, 1.0, 1.25), 1.75);
}

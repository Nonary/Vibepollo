/**
 * @file tests/unit/platform/linux/test_kmsgrab_selection.cpp
 * @brief Tests for stable Linux KMS output naming and selection.
 */
#include "../../../tests_common.h"

#include <limits>
#include <src/platform/linux/kmsgrab_selection.h>

namespace selection = platf::kms::selection;

TEST(KmsgrabSelection, RecognizesCudaImportableDisplayDrivers) {
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm-extra"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("vibeshine_drm"));

  EXPECT_FALSE(selection::driver_supports_cuda_import("vkms"));
  EXPECT_FALSE(selection::driver_supports_cuda_import("i915"));
  EXPECT_FALSE(selection::driver_supports_cuda_import(""));
  EXPECT_FALSE(selection::driver_is_nvidia("vibeshine_drm"));
  EXPECT_TRUE(selection::driver_requires_direct_import("vibeshine_drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("nvidia-drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("vkms"));
}

TEST(KmsgrabSelection, ParsesOnlyCompleteUnsignedNumericAliases) {
  EXPECT_EQ(selection::parse_numeric_alias("0"), 0u);
  EXPECT_EQ(selection::parse_numeric_alias("17"), 17u);
  EXPECT_EQ(selection::parse_numeric_alias(std::to_string(std::numeric_limits<std::uint32_t>::max())), std::numeric_limits<std::uint32_t>::max());

  EXPECT_FALSE(selection::parse_numeric_alias(""));
  EXPECT_FALSE(selection::parse_numeric_alias("-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("1-HDMI-A-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("4294967296"));
}

TEST(KmsgrabSelection, UsesStableConnectorNamesAndLegacyMonitorOrdinals) {
  const auto monitors = selection::name_monitors({
    {"card1", "HDMI-A-1", 52, 1},
    {"card2", "Virtual-1", 40, 0},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "Virtual-1");
  EXPECT_EQ(monitors[1].display_name, "HDMI-A-1");

  const auto stable_match = selection::resolve_named_monitor("virtual-1", monitors);
  ASSERT_TRUE(stable_match);
  EXPECT_EQ(stable_match->card_path, "card2");
  EXPECT_EQ(stable_match->crtc_id, 40u);
  EXPECT_EQ(stable_match->monitor_index, 0u);

  const auto numeric_alias = selection::parse_numeric_alias("1");
  ASSERT_TRUE(numeric_alias);
  EXPECT_EQ(*numeric_alias, monitors[1].monitor.monitor_index);
}

TEST(KmsgrabSelection, QualifiesCrossGpuCollisionsAndRejectsAmbiguousRawName) {
  const auto monitors = selection::name_monitors({
    {"card0", "HDMI-A-1", 10, 0},
    {"card1", "HDMI-A-1", 20, 1},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "card0:HDMI-A-1");
  EXPECT_EQ(monitors[1].display_name, "card1:HDMI-A-1");
  EXPECT_FALSE(selection::resolve_named_monitor("HDMI-A-1", monitors));

  const auto match = selection::resolve_named_monitor("CARD1:hdmi-a-1", monitors);
  ASSERT_TRUE(match);
  EXPECT_EQ(match->card_path, "card1");
  EXPECT_EQ(match->crtc_id, 20u);
}

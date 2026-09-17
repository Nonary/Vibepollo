/**
 * @file tests/unit/platform/windows/test_display_mode_resolution.cpp
 * @brief Regression tests for DXGI/GDI display-mode hybrid resolution.
 */
#include "../../../tests_common.h"

#include <display_device/windows/display_mode_resolution.h>

#include <optional>
#include <vector>

namespace {
  using display_device::DisplayMode;
  using display_device::Resolution;
  using display_device::win_utils::mergeDisplayModes;
  using display_device::win_utils::resolveRequestedDisplayMode;
  using display_device::win_utils::supportedModesContainResolution;

  const DisplayMode kCustomVddMode {3840, 1680, {60, 1}};
  const DisplayMode kCustomVddMode1690 {3840, 1690, {60, 1}};
  const DisplayMode kStockQhd {2560, 1440, {60, 1}};
  const DisplayMode kStock4k {3840, 2160, {60, 1}};
  const DisplayMode kStockSuperUltrawide {5120, 1440, {60, 1}};
  const DisplayMode kStock1080p {1920, 1080, {60, 1}};
  const DisplayMode kStock1080p120 {1920, 1080, {120, 1}};

  const std::vector<DisplayMode> kDxgiStockModes {
    kStock1080p,
    kStockQhd,
    kStock4k,
    kStockSuperUltrawide
  };

  bool sameMode(const DisplayMode &lhs, const DisplayMode &rhs) {
    return lhs.m_resolution.m_width == rhs.m_resolution.m_width &&
           lhs.m_resolution.m_height == rhs.m_resolution.m_height &&
           lhs.m_refresh_rate.m_numerator == rhs.m_refresh_rate.m_numerator &&
           lhs.m_refresh_rate.m_denominator == rhs.m_refresh_rate.m_denominator;
  }

  bool containsMode(const std::vector<DisplayMode> &modes, const DisplayMode &expected) {
    for (const auto &mode : modes) {
      if (sameMode(mode, expected)) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST(DisplayModeResolution, GdiCustomModeIsKeptWhenDxgiOmitsIt) {
  const auto merged {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};

  EXPECT_TRUE(containsMode(merged, kCustomVddMode));
  EXPECT_TRUE(containsMode(merged, kStockSuperUltrawide));
  EXPECT_TRUE(containsMode(merged, kStock4k));
  EXPECT_EQ(merged.size(), kDxgiStockModes.size() + 1);
}

TEST(DisplayModeResolution, DxgiStockModesDoNotContainCustomResolution) {
  EXPECT_FALSE(supportedModesContainResolution(kDxgiStockModes, kCustomVddMode.m_resolution));
  EXPECT_TRUE(supportedModesContainResolution(kDxgiStockModes, kStockSuperUltrawide.m_resolution));
}

TEST(DisplayModeResolution, HybridProbeMergesOnlyTheRequestedCustomMode) {
  // DXGI reports the stock table and omits 3840x1680. A targeted GDI probe
  // (CDS_TEST) confirms that one custom mode without walking the GDI list.
  ASSERT_FALSE(supportedModesContainResolution(kDxgiStockModes, kCustomVddMode.m_resolution));
  const auto supported {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, supported, Resolution {3840, 2160})};

  EXPECT_TRUE(sameMode(resolved, kCustomVddMode));
}

TEST(DisplayModeResolution, CustomModeFromGdiIsPreferredOverCloserPixelCount) {
  const auto supported {mergeDisplayModes(kDxgiStockModes, {kCustomVddMode})};
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, supported, Resolution {3840, 2160})};

  EXPECT_TRUE(sameMode(resolved, kCustomVddMode));
}

TEST(DisplayModeResolution, DoesNotChangeAspectRatioWhenCustomModeMissing) {
  // DXGI-only list: the old "closest pixel area" fallback would pick 5120x1440
  // because it is closer in total pixels than 3840x2160. Keep the requested mode.
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_EQ(resolved.m_resolution.m_width, 3840U);
  EXPECT_EQ(resolved.m_resolution.m_height, 1680U);
  EXPECT_FALSE(sameMode(resolved, kStockSuperUltrawide));
}

TEST(DisplayModeResolution, NearbyCustomHeightDoesNotSelectSuperUltrawide) {
  const auto resolved {resolveRequestedDisplayMode(kCustomVddMode1690, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_EQ(resolved.m_resolution.m_width, 3840U);
  EXPECT_EQ(resolved.m_resolution.m_height, 1690U);
  EXPECT_FALSE(sameMode(resolved, kStockSuperUltrawide));
}

TEST(DisplayModeResolution, SameAspectClosestIsUsedWhenExactMissing) {
  const DisplayMode requested {1600, 900, {60, 1}};
  const auto resolved {resolveRequestedDisplayMode(requested, kDxgiStockModes, Resolution {3840, 2160})};

  EXPECT_TRUE(sameMode(resolved, kStock1080p));
}

TEST(DisplayModeResolution, SameResolutionDifferentRefreshIsPreferred) {
  const std::vector<DisplayMode> supported {
    kStock1080p,
    kStock1080p120,
    kStockSuperUltrawide
  };
  const DisplayMode requested {1920, 1080, {144, 1}};
  const auto resolved {resolveRequestedDisplayMode(requested, supported, std::nullopt)};

  EXPECT_TRUE(sameMode(resolved, kStock1080p120));
}

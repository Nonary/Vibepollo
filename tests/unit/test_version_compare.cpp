/**
 * @file tests/unit/test_version_compare.cpp
 * @brief Unit tests for Vibepollo's semver comparison rules.
 */

#include "../tests_common.h"

#include <src/version_compare.h>

TEST(VersionCompareTest, StableRespinsSortAbovePlainRelease) {
  EXPECT_LT(version_compare::compare_semver("1.14.14", "1.14.14-stable.1"), 0);
  EXPECT_GT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14"), 0);
}

TEST(VersionCompareTest, StandardPrereleasesStayBelowRelease) {
  EXPECT_LT(version_compare::compare_semver("1.14.14-alpha.1", "1.14.14"), 0);
  EXPECT_LT(version_compare::compare_semver("1.14.14-beta.1", "1.14.14"), 0);
  EXPECT_LT(version_compare::compare_semver("1.14.14-rc.1", "1.14.14"), 0);
}

TEST(VersionCompareTest, StableRespinsStillCompareWithinTheirChannel) {
  EXPECT_LT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14-stable.2"), 0);
  EXPECT_GT(version_compare::compare_semver("1.14.14-stable.1", "1.14.14-rc.9"), 0);
  EXPECT_EQ(version_compare::compare_semver("1.14.14+build.1", "1.14.14+build.2"), 0);
}

TEST(VersionCompareTest, StableRespinsAreNotPrereleaseChannel) {
  EXPECT_FALSE(version_compare::is_prerelease_channel("1.15.4"));
  EXPECT_FALSE(version_compare::is_prerelease_channel("1.15.4-stable.3"));
  EXPECT_FALSE(version_compare::is_prerelease_channel("v1.15.4-Stable.3"));
  EXPECT_TRUE(version_compare::is_prerelease_channel("1.15.5-alpha.3"));
  EXPECT_TRUE(version_compare::is_prerelease_channel("1.15.5-beta.1"));
  EXPECT_TRUE(version_compare::is_prerelease_channel("1.15.5-rc.1"));
}

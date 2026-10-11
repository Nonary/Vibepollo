#include "../tests_common.h"

#include <src/color_profile_policy.h>

namespace {
  struct native_result_t {
    bool api_available;
    bool success;
  };
}

TEST(ColorProfilePolicy, AccessDeniedDoesNotLeaveRegistryOnlyProfile) {
  bool registry_associated = false;
  EXPECT_FALSE(color_profile_policy::associate(
    [] { return native_result_t {true, false}; },
    [&] { registry_associated = true; return true; }
  ));
  EXPECT_FALSE(registry_associated);
}

TEST(ColorProfilePolicy, SuccessfulActivationDoesNotDuplicateRegistryAssociation) {
  int writes = 0;
  EXPECT_TRUE(color_profile_policy::associate(
    [] { return native_result_t {true, true}; },
    [&] { ++writes; return true; }
  ));
  EXPECT_EQ(writes, 0);
}

TEST(ColorProfilePolicy, UnsupportedWindowsUsesLegacyAssociationOnce) {
  int writes = 0;
  EXPECT_TRUE(color_profile_policy::associate(
    [] { return native_result_t {false, false}; },
    [&] { ++writes; return true; }
  ));
  EXPECT_EQ(writes, 1);
}

TEST(ColorProfilePolicy, LegacyPermissionFailureRemainsFailure) {
  EXPECT_FALSE(color_profile_policy::associate(
    [] { return native_result_t {false, false}; },
    [] { return false; }
  ));
}

TEST(ColorProfilePolicy, ResolvedDefaultDriverMonitorOwnsCalibration) {
  for (const auto path : {
         LR"(\\?\DISPLAY#SDD4100#permanent#{id})",
         LR"(\\?\display#sdd4aBc#permanent#{id})",
         LR"(\\?\DISPLAY#SDD5000#temporary#{id})",
         LR"(\\?\DISPLAY#SDD5FFF#temporary#{id})"}) {
    EXPECT_TRUE(color_profile_policy::is_sunshine_driver_monitor_path(path));
  }
}

TEST(ColorProfilePolicy, PhysicalSudoAndMalformedPathsKeepExistingClassification) {
  for (const auto path : {
         LR"(\\?\DISPLAY#DEL1234#physical#{id})",
         LR"(\\?\DISPLAY#MTT1337#sudo#{id})",
         LR"(\\?\DISPLAY#SDD6100#custom#{id})",
         LR"(\\?\DISPLAY#SDD410#short#{id})",
         LR"(\\?\DISPLAY#SDD41000#long#{id})",
         LR"(\\?\DISPLAY#SDD4100X#suffix#{id})",
         LR"(\\?\DISPLAY#SDD4ZZZ#invalid#{id})",
         LR"(\\?\NOTDISPLAY#SDD4100#near#{id})",
         LR"(\\?\DISPLAY#DEL1234#SDD4100#{id})",
         LR"(\\?\DISPLAY#SDD4100)",
         L""}) {
    EXPECT_FALSE(color_profile_policy::is_sunshine_driver_monitor_path(path));
  }
}

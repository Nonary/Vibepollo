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

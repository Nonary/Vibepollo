/**
 * @file tests/unit/platform/linux/test_private_display_resume_policy.cpp
 * @brief Tests for Linux private-display resume policy.
 */
#include <gtest/gtest.h>

#include <src/platform/linux/private_display_resume_policy.h>

namespace policy = platf::linux_private_display::resume_policy;

TEST(LinuxPrivateDisplayResumePolicy, OnlyAnAppWithADisplayLeaseOverridesTheRequestingOwner) {
  EXPECT_EQ(policy::reservation_owner("mac", "deck", 12), "deck");
  EXPECT_EQ(policy::reservation_owner("mac", "deck", 0), "mac");
  EXPECT_EQ(policy::reservation_owner("mac", "", 12), "mac");
  EXPECT_EQ(policy::reservation_owner("deck", "deck", 12), "deck");
}

TEST(LinuxPrivateDisplayResumePolicy, ReappliesNewlyConnectedEnabledOutput) {
  EXPECT_TRUE(policy::requires_apply(true, true));
}

TEST(LinuxPrivateDisplayResumePolicy, ReappliesDisabledExistingOutput) {
  EXPECT_TRUE(policy::requires_apply(false, false));
}

TEST(LinuxPrivateDisplayResumePolicy, ReusesConfiguredExistingOutput) {
  EXPECT_FALSE(policy::requires_apply(false, true));
}

TEST(LinuxPrivateDisplayResumePolicy, RecomposesNewNormalGameIdentity) {
  EXPECT_TRUE(policy::requires_topology_reapply(true, false));
}

TEST(LinuxPrivateDisplayResumePolicy, RecomposesRecreatedRetainedOutput) {
  EXPECT_TRUE(policy::requires_topology_reapply(false, true));
}

TEST(LinuxPrivateDisplayResumePolicy, ReusesRetainedConfiguredTopology) {
  EXPECT_FALSE(policy::requires_topology_reapply(false, false));
}

TEST(LinuxPrivateDisplayResumePolicy, ReusesReadyPrivateDisplayDespiteLegacyApplyGate) {
  EXPECT_FALSE(policy::requires_session_apply(true, true, false, false));
}

TEST(LinuxPrivateDisplayResumePolicy, AppliesNewOrRecreatedPrivateDisplay) {
  EXPECT_TRUE(policy::requires_session_apply(true, false, true, false));
  EXPECT_TRUE(policy::requires_session_apply(true, false, false, true));
}

TEST(LinuxPrivateDisplayResumePolicy, PreservesPhysicalDisplayApplyGate) {
  EXPECT_TRUE(policy::requires_session_apply(false, true, false, false));
  EXPECT_FALSE(policy::requires_session_apply(false, false, true, true));
}

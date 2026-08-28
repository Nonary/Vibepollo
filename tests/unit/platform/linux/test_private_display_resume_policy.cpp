/**
 * @file tests/unit/platform/linux/test_private_display_resume_policy.cpp
 * @brief Tests for Linux private-display resume policy.
 */
#include <gtest/gtest.h>

#include <src/platform/linux/private_display_resume_policy.h>

namespace policy = platf::linux_private_display::resume_policy;

TEST(LinuxPrivateDisplayResumePolicy, ReappliesNewlyConnectedEnabledOutput) {
  EXPECT_TRUE(policy::requires_apply(true, true));
}

TEST(LinuxPrivateDisplayResumePolicy, ReappliesDisabledExistingOutput) {
  EXPECT_TRUE(policy::requires_apply(false, false));
}

TEST(LinuxPrivateDisplayResumePolicy, ReusesConfiguredExistingOutput) {
  EXPECT_FALSE(policy::requires_apply(false, true));
}

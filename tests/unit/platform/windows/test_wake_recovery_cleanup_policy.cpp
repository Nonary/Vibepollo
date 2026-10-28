#include <gtest/gtest.h>
#include <src/platform/windows/wake_recovery_cleanup_policy.h>

namespace policy = platf::wake_recovery_cleanup_policy;

TEST(WakeRecoveryCleanupPolicy, RequiresFailedCurrentTicketFreshVerificationAndNoOwners) {
  policy::state_t state;
  ASSERT_TRUE(state.begin(7, {"display-a", "display-b"}));
  ASSERT_TRUE(state.set_event_baseline(7, 2));
  ASSERT_TRUE(state.note_external_event(7, 3, false)); // Keep the hint while recovery is active.
  ASSERT_TRUE(state.update_status(7, policy::helper_status_t::failed));

  EXPECT_FALSE(state.authorize_target(7, 3, "display-a", false, true, true));
  EXPECT_FALSE(state.authorize_target(7, 3, "display-a", true, false, true));
  EXPECT_FALSE(state.authorize_target(7, 3, "display-a", true, true, false));
  EXPECT_TRUE(state.authorize_target(7, 3, "display-a", true, true, true));
}

TEST(WakeRecoveryCleanupPolicy, StaleTicketsSelfEventsAndUncapturedTargetsAreRejected) {
  policy::state_t state;
  ASSERT_TRUE(state.begin(12, {"captured-display"}));
  ASSERT_TRUE(state.set_event_baseline(12, 5));
  ASSERT_TRUE(state.update_status(12, policy::helper_status_t::failed));
  EXPECT_FALSE(state.note_external_event(11, 1, false));
  EXPECT_FALSE(state.note_external_event(12, 6, true));
  ASSERT_TRUE(state.note_external_event(12, 7, false));
  EXPECT_FALSE(state.authorize_target(12, 7, "newly-discovered-display", true, true, true));
  EXPECT_FALSE(state.authorize_target(11, 7, "captured-display", true, true, true));
}

TEST(WakeRecoveryCleanupPolicy, UnknownAndRestoredStatusesNeverAuthorizeCleanup) {
  policy::state_t state;
  ASSERT_TRUE(state.begin(3, {"display-a"}));
  EXPECT_FALSE(state.update_status(3, policy::helper_status_t::unknown));
  ASSERT_TRUE(state.set_event_baseline(3, 1));
  ASSERT_TRUE(state.note_external_event(3, 2, false));
  EXPECT_FALSE(state.authorize_target(3, 2, "display-a", true, true, true));
  ASSERT_TRUE(state.update_status(3, policy::helper_status_t::restored));
  EXPECT_FALSE(state.update_status(3, policy::helper_status_t::failed));
  EXPECT_FALSE(state.note_external_event(3, 2, false));
}

TEST(WakeRecoveryCleanupPolicy, MutationIsAttemptedOncePerCapturedIdentityAndIncident) {
  policy::state_t state;
  ASSERT_TRUE(state.begin(21, {"display-a", "display-b"}));
  ASSERT_TRUE(state.set_event_baseline(21, 0));
  ASSERT_TRUE(state.update_status(21, policy::helper_status_t::failed));
  ASSERT_TRUE(state.note_external_event(21, 1, false));
  ASSERT_TRUE(state.authorize_target(21, 1, "display-a", true, true, true));
  state.record_attempt(21, "display-a");
  EXPECT_FALSE(state.authorize_target(21, 1, "display-a", true, true, true));
  ASSERT_TRUE(state.note_external_event(21, 2, false));
  EXPECT_FALSE(state.authorize_target(21, 2, "display-a", true, true, true));
  EXPECT_TRUE(state.authorize_target(21, 2, "display-b", true, true, true));
}

TEST(WakeRecoveryCleanupPolicy, NewerTicketReplacesFailureAndRejectsOldEvents) {
  policy::state_t state;
  ASSERT_TRUE(state.begin(44, {"old-display"}));
  ASSERT_TRUE(state.set_event_baseline(44, 0));
  ASSERT_TRUE(state.update_status(44, policy::helper_status_t::failed));
  ASSERT_TRUE(state.begin(45, {"new-display"}));
  ASSERT_TRUE(state.set_event_baseline(45, 2));
  EXPECT_FALSE(state.note_external_event(44, 1, false));
  EXPECT_FALSE(state.update_status(44, policy::helper_status_t::failed));
  EXPECT_FALSE(state.note_external_event(45, 2, false));
}

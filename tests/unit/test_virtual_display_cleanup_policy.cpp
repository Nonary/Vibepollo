/**
 * @file tests/unit/test_virtual_display_cleanup_policy.cpp
 * @brief Pure retained-display cleanup and restore-order contracts.
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  #include <src/platform/windows/virtual_display_cleanup.h>
  #include <src/platform/windows/virtual_display_policy.h>

TEST(VirtualDisplayCleanupPolicy, RestoreBeforeRemoveKeepsHelperFirst) {
  const auto steps = platf::virtual_display_cleanup::ordered_restore_steps(
    platf::virtual_display_cleanup::revert_order_t::restore_before_remove
  );
  EXPECT_EQ(steps[0], platf::virtual_display_cleanup::cleanup_step_t::helper_revert);
  EXPECT_EQ(steps[1], platf::virtual_display_cleanup::cleanup_step_t::retained_probe_remove);
  EXPECT_EQ(steps[2], platf::virtual_display_cleanup::cleanup_step_t::explicit_display_remove);
  EXPECT_EQ(steps[3], platf::virtual_display_cleanup::cleanup_step_t::database_restore);
}

TEST(VirtualDisplayCleanupPolicy, RemoveBeforeRestoreKeepsTeardownOnlyOrder) {
  const auto steps = platf::virtual_display_cleanup::ordered_restore_steps(
    platf::virtual_display_cleanup::revert_order_t::remove_before_restore
  );
  EXPECT_EQ(steps[0], platf::virtual_display_cleanup::cleanup_step_t::retained_probe_remove);
  EXPECT_EQ(steps[1], platf::virtual_display_cleanup::cleanup_step_t::explicit_display_remove);
  EXPECT_EQ(steps[2], platf::virtual_display_cleanup::cleanup_step_t::helper_revert);
  EXPECT_EQ(steps[3], platf::virtual_display_cleanup::cleanup_step_t::database_restore);
}

TEST(VirtualDisplayCleanupPolicy, SunshineLeaseOwnedGuidSurvivesMissingWindowsEnumeration) {
  EXPECT_FALSE(VDISPLAY::policy::retained_target_is_owned(false, false));
  EXPECT_TRUE(VDISPLAY::policy::retained_target_is_owned(false, true));
  EXPECT_TRUE(VDISPLAY::policy::retained_target_is_owned(true, false));
}

TEST(VirtualDisplayCleanupPolicy, SudoVdaAcceptedProvenanceOwnsUnenumeratedGuid) {
  // SudoVDA has no Sunshine lease tracker; accepted render-adapter
  // provenance is the ownership signal until Windows publishes the target.
  EXPECT_TRUE(VDISPLAY::policy::retained_target_is_owned(false, true));
}

TEST(VirtualDisplayCleanupPolicy, CompletedProbeAlwaysReleasesItsTemporaryDisplay) {
  EXPECT_TRUE(VDISPLAY::policy::should_cleanup_temporary_probe(true));
  EXPECT_FALSE(VDISPLAY::policy::should_cleanup_temporary_probe(false));
}

TEST(VirtualDisplayCleanupPolicy, SharedProbeDisplayIsRemovedByItsLastUser) {
  VDISPLAY::policy::probe_display_lifetime_t lifetime;
  const auto first = lifetime.begin_lifetime();
  const auto second = lifetime.acquire();

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(lifetime.active_probes(), 2u);
  EXPECT_FALSE(lifetime.begin_idle_removal());
  EXPECT_EQ(
    lifetime.release(*first),
    VDISPLAY::policy::probe_display_release_action::retained_for_other_probe
  );
  EXPECT_TRUE(lifetime.retained());
  EXPECT_EQ(
    lifetime.release(*second),
    VDISPLAY::policy::probe_display_release_action::remove
  );
  EXPECT_TRUE(lifetime.removal_in_progress());
  EXPECT_FALSE(lifetime.acquire());
  lifetime.complete_removal(*second, true);
  EXPECT_FALSE(lifetime.retained());
}

TEST(VirtualDisplayCleanupPolicy, FailedRemovalCanBeRetriedWithoutAcceptingStaleCleanup) {
  VDISPLAY::policy::probe_display_lifetime_t lifetime;
  const auto first = lifetime.begin_lifetime();
  ASSERT_TRUE(first);
  ASSERT_EQ(
    lifetime.release(*first),
    VDISPLAY::policy::probe_display_release_action::remove
  );

  lifetime.complete_removal(*first, false);
  const auto idle_retry = lifetime.begin_idle_removal();
  ASSERT_TRUE(idle_retry);
  EXPECT_EQ(*idle_retry, *first);
  lifetime.complete_removal(*idle_retry, false);
  const auto retry = lifetime.acquire();
  ASSERT_TRUE(retry);
  EXPECT_EQ(*retry, *first);
  EXPECT_EQ(lifetime.release(*first), VDISPLAY::policy::probe_display_release_action::remove);
  lifetime.complete_removal(*first, true);
  EXPECT_FALSE(lifetime.retained());

  const auto replacement = lifetime.begin_lifetime();
  ASSERT_TRUE(replacement);
  EXPECT_NE(*replacement, *first);
  EXPECT_EQ(
    lifetime.release(*first),
    VDISPLAY::policy::probe_display_release_action::ignored
  );
  EXPECT_EQ(lifetime.active_probes(), 1u);
}

TEST(VirtualDisplayCleanupPolicy, DatabaseFallbackRemainsAfterVirtualCleanup) {
  const auto steps = platf::virtual_display_cleanup::ordered_restore_steps(
    platf::virtual_display_cleanup::revert_order_t::restore_before_remove
  );
  // Database restore remains the fallback after helper dispatch is attempted;
  // retained-display removal must not replace it.
  EXPECT_EQ(steps.back(), platf::virtual_display_cleanup::cleanup_step_t::database_restore);
}
#endif  // _WIN32

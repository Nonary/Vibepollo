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

TEST(VirtualDisplayCleanupPolicy, RetainedProbeTransfersToFirstSession) {
  // Before the handoff policy, a successful idle HTTP probe retained
  // "Sunshine Temporary" after a first physical or virtual session started.
  EXPECT_TRUE(VDISPLAY::policy::should_handoff_retained_probe_display(
    true,
    true,
    false
  ));
}

TEST(VirtualDisplayCleanupPolicy, RetainedProbeStaysOutsideFirstSessionHandoff) {
  // Do not remove an idle probe while another active or pending session owns
  // the lifecycle, when no retained probe exists, or while physical restore is
  // in progress.
  EXPECT_FALSE(VDISPLAY::policy::should_handoff_retained_probe_display(
    false,
    true,
    false
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_handoff_retained_probe_display(
    true,
    false,
    false
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_handoff_retained_probe_display(
    true,
    true,
    true
  ));
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

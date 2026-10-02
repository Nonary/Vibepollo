/**
 * @file tests/unit/test_virtual_display_cleanup_policy.cpp
 * @brief Pure retained-display cleanup and restore-order contracts.
 */
#include <gtest/gtest.h>
#include <src/framegen_policy.h>
#include <src/platform/windows/virtual_display_policy.h>

#include <optional>
#include <string>

TEST(VirtualDisplayCleanupPolicy, RetainedResumeRecoveryExcludesLiveAndSecondaryPeers) {
  EXPECT_TRUE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    true,
    false,
    true,
    41,
    "retained-owner"
  ));
  // An existing capture is a live peer; its worker must not be replaced.
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    false,
    true,
    false,
    true,
    41,
    "retained-owner"
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    true,
    true,
    true,
    41,
    "retained-owner"
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    false,
    false,
    true,
    41,
    "retained-owner"
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    true,
    false,
    false,
    41,
    "retained-owner"
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    true,
    false,
    true,
    0,
    "retained-owner"
  ));
  EXPECT_FALSE(VDISPLAY::policy::should_rearm_retained_game_output_recovery(
    true,
    true,
    false,
    true,
    41,
    ""
  ));
}

TEST(VirtualDisplayCleanupPolicy, RecoveryRefreshPreservesVibepolloMillihertzBoundary) {
  const auto fallback = framegen::make_virtual_display_recovery_refresh({
    .stream_fps_millihz = 60000,
    .effective_display_refresh_millihz = 60000,
  });
  EXPECT_EQ(fallback.base_fps_millihz, 60000u);
  EXPECT_EQ(fallback.fps_millihz, 60000u);

  const auto client_override = framegen::make_virtual_display_recovery_refresh({
    .client_display_refresh_millihz = 59940,
    .stream_fps_millihz = 60000,
    .effective_display_refresh_millihz = 59940,
  });
  EXPECT_EQ(client_override.base_fps_millihz, 59940u);
  EXPECT_EQ(client_override.fps_millihz, 59940u);

  const auto multiplied = framegen::make_virtual_display_recovery_refresh({
    .client_display_refresh_millihz = 59940,
    .stream_fps_millihz = 60000,
    .effective_display_refresh_millihz = 59940,
    .refresh_multiplier = 4,
  });
  EXPECT_EQ(multiplied.base_fps_millihz, 59940u);
  EXPECT_EQ(multiplied.fps_millihz, 239760u);
}

TEST(VirtualDisplayCleanupPolicy, PausedRetainedResumeRearmsBothReusePathsOnlyAfterAdmission) {
  struct recovery_context_t {
    std::string device_id;
    unsigned int width;
    unsigned int height;
    unsigned int refresh_millihz;
    bool hdr;
    std::string layout;
    std::string display_configuration;

    bool operator==(const recovery_context_t &) const = default;
  };

  const recovery_context_t retained_mode {
    .device_id = R"(\\.\DISPLAY17)",
    .width = 2560,
    .height = 1440,
    .refresh_millihz = 117'500,
    .hdr = true,
    .layout = "extended",
    .display_configuration = "ensure-active",
  };
  bool recovery_armed = false;
  std::optional<recovery_context_t> scheduled_context;

  {
    VDISPLAY::policy::retained_resume_recovery_rearm_t rejected_resume;
    rejected_resume.stage(VDISPLAY::policy::retained_resume_reuse_path_e::capture_ready_output, [&] {
      recovery_armed = true;
    });
    EXPECT_TRUE(rejected_resume.pending());
    EXPECT_EQ(
      rejected_resume.path(),
      VDISPLAY::policy::retained_resume_reuse_path_e::capture_ready_output
    );
    EXPECT_FALSE(recovery_armed);
    EXPECT_FALSE(rejected_resume.admit_and_commit([] {
      return false;
    }));
    EXPECT_FALSE(rejected_resume.pending());
    EXPECT_FALSE(rejected_resume.path());
  }
  EXPECT_FALSE(recovery_armed);

  for (const auto reuse_path : {
         VDISPLAY::policy::retained_resume_reuse_path_e::prepared_existing_display,
         VDISPLAY::policy::retained_resume_reuse_path_e::capture_ready_output,
       }) {
    VDISPLAY::policy::retained_resume_recovery_rearm_t accepted_resume;
    bool pending_owner_published = false;
    recovery_armed = false;
    scheduled_context.reset();
    auto live_session_context = retained_mode;
    accepted_resume.stage(reuse_path, [&, recovery_snapshot = live_session_context] {
      EXPECT_TRUE(pending_owner_published);
      recovery_armed = true;
      scheduled_context = recovery_snapshot;
    });
    // The production retained-output join path clears creation-only fields
    // after staging. The recovery action must retain the seeded app/client
    // snapshot rather than the normalized live join session.
    live_session_context.layout = "exclusive";
    live_session_context.display_configuration = "global-default";
    EXPECT_TRUE(accepted_resume.pending());
    EXPECT_EQ(accepted_resume.path(), reuse_path);
    EXPECT_FALSE(recovery_armed);
    EXPECT_TRUE(accepted_resume.admit_and_commit([&] {
      pending_owner_published = true;
      return true;
    }));
    EXPECT_TRUE(recovery_armed);
    EXPECT_EQ(scheduled_context, retained_mode);
    EXPECT_FALSE(accepted_resume.pending());
    EXPECT_FALSE(accepted_resume.path());
  }

  VDISPLAY::policy::retained_resume_recovery_rearm_t no_rearm;
  bool admitted_without_rearm = false;
  EXPECT_TRUE(no_rearm.admit_and_commit([&] {
    admitted_without_rearm = true;
    return true;
  }));
  EXPECT_TRUE(admitted_without_rearm);
}

#ifdef _WIN32
  #include <src/platform/windows/virtual_display.h>
  #include <src/platform/windows/virtual_display_cleanup.h>

TEST(VirtualDisplayCleanupPolicy, OwnedProbeRequestDoesNotImplyCaptureReadiness) {
  const VDISPLAY::ensure_display_result result {
    .readiness = VDISPLAY::ensure_display_readiness_e::request_retained,
    .tracks_temporary_for_probe = true,
    .temporary_generation = 1,
  };

  EXPECT_TRUE(result.owns_temporary_probe_request());
  EXPECT_FALSE(result.ready_for_capture());
}

TEST(VirtualDisplayCleanupPolicy, ExactPublishedTargetIsCaptureReady) {
  const VDISPLAY::ensure_display_result result {
    .readiness = VDISPLAY::ensure_display_readiness_e::target_ready,
    .tracks_temporary_for_probe = true,
    .temporary_generation = 1,
    .display_name = R"(\\.\DISPLAY55)",
  };

  EXPECT_TRUE(result.owns_temporary_probe_request());
  EXPECT_TRUE(result.ready_for_capture());
}

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

TEST(VirtualDisplayCleanupPolicy, OnlyTerminalUserActionOverridesManagedOwnership) {
  using platf::virtual_display_cleanup::cleanup_admission_policy_t;
  using platf::virtual_display_cleanup::cleanup_admitted;

  EXPECT_TRUE(cleanup_admitted(true, cleanup_admission_policy_t::respect_managed_owners));
  EXPECT_FALSE(cleanup_admitted(false, cleanup_admission_policy_t::respect_managed_owners));
  EXPECT_TRUE(cleanup_admitted(false, cleanup_admission_policy_t::override_managed_owners));
}

TEST(VirtualDisplayCleanupPolicy, IdleEndedOrPausedStreamDisengagesRecoveryMonitor) {
  using platf::virtual_display_cleanup::idle_stream_cleanup_recovery_policy;
  using platf::virtual_display_cleanup::idle_stream_disengages_recovery_monitor;
  using platf::virtual_display_cleanup::recovery_monitor_policy_t;

  EXPECT_TRUE(idle_stream_disengages_recovery_monitor(true));
  EXPECT_FALSE(idle_stream_disengages_recovery_monitor(false));
  EXPECT_EQ(
    idle_stream_cleanup_recovery_policy(),
    recovery_monitor_policy_t::disengage_before_admission
  );
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

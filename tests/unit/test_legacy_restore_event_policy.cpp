#include <gtest/gtest.h>
#include <src/platform/windows/legacy_restore_event_policy.h>

namespace policy = display_helper::legacy_restore_event_policy;

TEST(LegacyRestoreEventPolicy, FailedStageAndNewRequiredPhysicalPresenceAuthorizeRetry) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(12, {"physical-a", "physical-b"}, {"physical-a"});
  ASSERT_TRUE(state.note_display_event(12));
  ASSERT_TRUE(state.finish_attempt(12, true));
  EXPECT_TRUE(state.reconcile(12, 12, {"physical-a", "physical-b"}));
  EXPECT_FALSE(state.pending());
}

TEST(LegacyRestoreEventPolicy, EventWithoutFailedStageOrNewRequiredPresenceDoesNotRetry) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(4, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(4));
  EXPECT_FALSE(state.finish_attempt(4, false));

  state.begin_attempt(5, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(5));
  ASSERT_TRUE(state.finish_attempt(5, true));
  EXPECT_FALSE(state.reconcile(5, 5, {}));

  state.begin_attempt(6, {"physical-a"}, {"physical-a"});
  ASSERT_TRUE(state.note_display_event(6));
  ASSERT_TRUE(state.finish_attempt(6, true));
  EXPECT_FALSE(state.reconcile(6, 6, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, UnknownBaselineObservationCannotAuthorizeRetry) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(8, {"physical-a"}, {}, false);
  EXPECT_TRUE(state.note_display_event(8)); // Retain the hint, but do not authorize it.
  EXPECT_FALSE(state.finish_attempt(8, true));
  EXPECT_FALSE(state.reconcile(8, 8, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, UnknownFollowupObservationCanBeDiscardedWithoutRetry) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(9, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(9));
  ASSERT_TRUE(state.finish_attempt(9, true));
  state.discard_pending(9);
  EXPECT_FALSE(state.pending());
  EXPECT_FALSE(state.reconcile(9, 9, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, InitialZeroGenerationIsAValidRestoreEpoch) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(0, {"physical-a"}, {});
  ASSERT_TRUE(state.active());
  ASSERT_TRUE(state.note_display_event(0));
  ASSERT_TRUE(state.finish_attempt(0, true));
  EXPECT_TRUE(state.reconcile(0, 0, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, EventsDuringObservationAreGuardedAndPendingBurstsCannotBypassReconciliation) {
  policy::deferred_physical_return_t state;
  state.begin_observation(0);
  EXPECT_TRUE(state.note_display_event(0)); // Retained as a hint while observation is prepared.
  ASSERT_TRUE(state.install_observations(0, {"physical-a"}, {}, true));
  ASSERT_TRUE(state.finish_attempt(0, true));
  EXPECT_TRUE(state.note_display_event(0)); // Absorb later feedback while this hint is pending.
  EXPECT_FALSE(state.reconcile(0, 0, {"unrelated-device"}));
  EXPECT_FALSE(state.pending());

  state.begin_attempt(1, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(1));
  ASSERT_TRUE(state.finish_attempt(1, true));
  EXPECT_TRUE(state.note_display_event(1));
  EXPECT_TRUE(state.reconcile(1, 1, {"physical-a"}));
  EXPECT_TRUE(state.note_display_event(1)); // Events during the admitted attempt cannot queue a duplicate.
  EXPECT_FALSE(state.reconcile(1, 1, {"physical-a"})); // A confirmed return is consumed once.
}

TEST(LegacyRestoreEventPolicy, PreparationEventCanAuthorizeOnlyAfterKnownBaselineAndFailedStage) {
  policy::deferred_physical_return_t state;
  state.begin_observation(44);
  ASSERT_TRUE(state.note_display_event(44)); // Before observation installation, after baseline sample.
  ASSERT_TRUE(state.install_observations(44, {"physical-a"}, {}, true));
  ASSERT_TRUE(state.finish_attempt(44, true));
  EXPECT_TRUE(state.reconcile(44, 44, {"physical-a"}));
  EXPECT_FALSE(state.reconcile(44, 44, {"physical-a"}));

  state.begin_observation(45);
  ASSERT_TRUE(state.note_display_event(45));
  ASSERT_TRUE(state.install_observations(45, {"physical-a"}, {}, false));
  EXPECT_FALSE(state.finish_attempt(45, true));
  EXPECT_FALSE(state.reconcile(45, 45, {"physical-a"}));

  state.begin_observation(46);
  ASSERT_TRUE(state.note_display_event(46)); // Return already included in the before sample.
  ASSERT_TRUE(state.install_observations(46, {"physical-a"}, {"physical-a"}, true));
  ASSERT_TRUE(state.finish_attempt(46, true));
  EXPECT_FALSE(state.reconcile(46, 46, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, NonBaselinePresenceAndStaleGenerationAreRejected) {
  policy::deferred_physical_return_t state;
  state.begin_attempt(20, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(20));
  ASSERT_TRUE(state.finish_attempt(20, true));
  EXPECT_FALSE(state.reconcile(20, 21, {"unrelated-device"}));
  EXPECT_TRUE(state.pending());
  EXPECT_FALSE(state.reconcile(21, 21, {"physical-a"}));
  EXPECT_FALSE(state.reconcile(20, 20, {"unrelated-device"}));
}

TEST(LegacyRestoreEventPolicy, CancellationClearsDeferredEventAndInactivePresenceCounts) {
  policy::deferred_physical_return_t state;
  state.begin_observation(30);
  ASSERT_TRUE(state.note_display_event(30));
  state.clear();
  EXPECT_FALSE(state.finish_attempt(30, true));
  EXPECT_FALSE(state.reconcile(30, 30, {"physical-a"}));

  state.begin_attempt(31, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(31));
  ASSERT_TRUE(state.finish_attempt(31, true));
  state.clear();
  EXPECT_FALSE(state.reconcile(31, 32, {"physical-a"}));

  // The caller's set is enumAvailableDevices presence, including inactive but
  // connected devices; policy intentionally does not require an active mode.
  state.begin_attempt(33, {"physical-a"}, {});
  ASSERT_TRUE(state.note_display_event(33));
  ASSERT_TRUE(state.finish_attempt(33, true));
  EXPECT_TRUE(state.reconcile(33, 33, {"physical-a"}));
}

TEST(LegacyRestoreEventPolicy, ExhaustedWindowOpensOneBoundedRetryWhileActiveWindowKeepsItsBackoff) {
  EXPECT_EQ(policy::retry_action(true, false), policy::retry_action_t::open_bounded_event_window);
  EXPECT_EQ(policy::retry_action(true, true), policy::retry_action_t::join_open_window);
  EXPECT_EQ(policy::retry_action(false, false), policy::retry_action_t::none);
}

TEST(LegacyRestoreEventPolicy, JoinedHintDoesNotStartStageAfterWindowExpiresDuringBackoff) {
  // The active window may expire while await_restore_backoff is sleeping.
  constexpr std::int64_t existing_window_deadline_ms = 5000;
  EXPECT_TRUE(policy::hint_retry_admitted(existing_window_deadline_ms, 4900));
  EXPECT_FALSE(policy::hint_retry_admitted(existing_window_deadline_ms, 5100));
  EXPECT_FALSE(policy::hint_retry_admitted(0, 100));
}

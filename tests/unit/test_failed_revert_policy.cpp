#include <gtest/gtest.h>
#include <src/platform/windows/failed_revert_policy.h>

namespace policy = display_helper::failed_revert_policy;

namespace {
  constexpr policy::host_state_t idle {};

  policy::request_t final_stream_restore() {
    policy::request_t request;
    request.prefer_golden_if_current_missing = true;
    request.remove_display_after_restore = true;
    return request;
  }

  policy::request_t paused_game_restore() {
    policy::request_t request;
    request.retain_paused_app = true;
    request.issued_with_app_running = true;
    return request;
  }

  policy::request_t disconnect_restore_while_paused() {
    policy::request_t request;
    request.issued_with_app_running = true;
    return request;
  }
}  // namespace

TEST(FailedRevertPolicy, ReplaysTheMissedRevertWithItsOriginalArgumentsOnce) {
  policy::slot_t slot;
  policy::request_t missed;
  missed.prefer_golden_if_current_missing = false;
  missed.override_managed_ownership = true;
  missed.allow_disabled_recovery = true;
  missed.remove_display_after_restore = true;
  slot.remember(missed);
  ASSERT_TRUE(slot.pending());

  const auto replay = slot.take_if_replayable(idle);
  ASSERT_TRUE(replay.has_value());
  EXPECT_FALSE(replay->prefer_golden_if_current_missing);
  EXPECT_TRUE(replay->override_managed_ownership);
  EXPECT_TRUE(replay->allow_disabled_recovery);
  EXPECT_FALSE(replay->retain_paused_app);
  EXPECT_TRUE(replay->remove_display_after_restore);

  EXPECT_FALSE(slot.pending());
  EXPECT_FALSE(slot.take_if_replayable(idle).has_value());
}

TEST(FailedRevertPolicy, StreamOrLaunchKeepsTheRevertPending) {
  policy::slot_t slot;
  slot.remember(final_stream_restore());

  policy::host_state_t streaming;
  streaming.stream_active_or_pending = true;
  EXPECT_FALSE(slot.take_if_replayable(streaming).has_value());

  policy::host_state_t launching;
  launching.launch_pending = true;
  EXPECT_FALSE(slot.take_if_replayable(launching).has_value());

  EXPECT_TRUE(slot.pending());
  EXPECT_TRUE(slot.take_if_replayable(idle).has_value());
}

TEST(FailedRevertPolicy, RunningAppBlocksAnEndedStreamRestore) {
  policy::slot_t slot;
  slot.remember(final_stream_restore());

  policy::host_state_t app_running;
  app_running.app_running = true;
  EXPECT_FALSE(slot.take_if_replayable(app_running).has_value());
  EXPECT_TRUE(slot.pending());
}

TEST(FailedRevertPolicy, PausedGameRestoreReplaysWhileTheGameAwaitsResume) {
  policy::slot_t slot;
  slot.remember(paused_game_restore());

  policy::host_state_t paused;
  paused.app_running = true;
  const auto replay = slot.take_if_replayable(paused);
  ASSERT_TRUE(replay.has_value());
  EXPECT_TRUE(replay->retain_paused_app);

  slot.remember(paused_game_restore());
  paused.stream_active_or_pending = true;
  EXPECT_FALSE(slot.take_if_replayable(paused).has_value());
}

TEST(FailedRevertPolicy, RevertIssuedWhilePausedReplaysWithTheAppStillPaused) {
  policy::slot_t slot;
  slot.remember(disconnect_restore_while_paused());

  policy::host_state_t paused;
  paused.app_running = true;
  const auto replay = slot.take_if_replayable(paused);
  ASSERT_TRUE(replay.has_value());
  EXPECT_FALSE(replay->retain_paused_app);
}

TEST(FailedRevertPolicy, ApplyClearsAndALaterFailureReplacesTheMissedRevert) {
  policy::slot_t slot;
  slot.remember(paused_game_restore());
  slot.clear();
  EXPECT_FALSE(slot.pending());
  EXPECT_FALSE(slot.take_if_replayable(idle).has_value());

  slot.remember(paused_game_restore());
  slot.remember(final_stream_restore());
  const auto replay = slot.take_if_replayable(idle);
  ASSERT_TRUE(replay.has_value());
  EXPECT_FALSE(replay->retain_paused_app);
  EXPECT_TRUE(replay->remove_display_after_restore);
}

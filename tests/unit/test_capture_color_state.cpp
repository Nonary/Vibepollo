#include "../tests_common.h"
#include "src/capture_color_state.h"

using video::capture_color::transition_t;
using video::capture_color::action_e;
using namespace std::chrono_literals;

TEST(CaptureColorState, SustainedManualSdrInvalidatesHdrCapture) {
  transition_t state(true);
  const auto now = transition_t::clock::time_point {};
  EXPECT_EQ(state.observed(false, now), action_e::hold);
  EXPECT_EQ(state.observed(false, now + 250ms), action_e::reinit);
}

TEST(CaptureColorState, ExternalHdrChangeDoesNotDependOnTextureFormat) {
  transition_t state(false);
  const auto now = transition_t::clock::time_point {};
  state.notified(now, true);
  EXPECT_EQ(state.observed(false, now), action_e::hold); // stale DXGI
  EXPECT_EQ(state.observed(true, now + 50ms), action_e::hold);
  EXPECT_EQ(state.observed(true, now + 300ms), action_e::reinit);
}

TEST(CaptureColorState, UnknownIsNeverMisclassifiedAsSdr) {
  transition_t state(true);
  const auto now = transition_t::clock::time_point {};
  EXPECT_EQ(state.observed(std::nullopt, now), action_e::hold);
  EXPECT_EQ(state.observed(true, now + 1s), action_e::retain);
  EXPECT_FALSE(state.validation_pending());
}

TEST(CaptureColorState, FailedObservationAndStaleNotificationHaveFiniteRecovery) {
  transition_t state(true);
  const auto now = transition_t::clock::time_point {};
  EXPECT_EQ(state.observed(std::nullopt, now), action_e::hold);
  EXPECT_EQ(state.observed(std::nullopt, now + 2s), action_e::reinit);
  transition_t other(false);
  other.notified(now, true);
  EXPECT_EQ(other.observed(false, now + 2s), action_e::reinit);
}

TEST(CaptureColorState, TransientDoesNotChangeCommittedCaptureState) {
  transition_t state(true);
  const auto now = transition_t::clock::time_point {};
  EXPECT_EQ(state.observed(false, now), action_e::hold);
  EXPECT_EQ(state.observed(true, now + 100ms), action_e::retain);
  EXPECT_EQ(state.observed(false, now + 200ms), action_e::hold);
  EXPECT_EQ(state.observed(false, now + 400ms), action_e::hold);
  EXPECT_EQ(state.observed(false, now + 450ms), action_e::reinit);
}

TEST(CaptureColorState, OwnedMutationHoldsUntilCompletionThenInvalidatesGeneration) {
  transition_t state(true);
  EXPECT_EQ(state.mutation(10, false, false), action_e::retain);
  EXPECT_EQ(state.mutation(11, true, false), action_e::hold);
  // A one-second intentional SDR blank cannot change the accepted stream mode.
  EXPECT_EQ(state.mutation(11, true, false), action_e::hold);
  EXPECT_EQ(state.mutation(12, false, false), action_e::reinit);
}

TEST(CaptureColorState, FailedMutationEndsCaptureAndSameGenerationIsHarmless) {
  transition_t state(true);
  EXPECT_EQ(state.mutation(10, false, false), action_e::retain);
  EXPECT_EQ(state.mutation(10, false, false), action_e::retain);
  EXPECT_EQ(state.mutation(11, true, true), action_e::error);
}

TEST(CaptureColorState, UnrelatedColorNotificationDoesNotResetOrPauseHdr) {
  transition_t state(true);
  const auto now = transition_t::clock::time_point {};
  state.notified(now, true);
  EXPECT_EQ(state.observed(true, now), action_e::retain);
}

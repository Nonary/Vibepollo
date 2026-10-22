#include "../tests_common.h"
#include "src/capture_recovery.h"

TEST(CaptureRecovery, RequiresANewInstanceOfTheExactOutput) {
  video::capture_recovery::observations_t state;
  const auto old = state.begin_capture();
  const auto watch = state.watch({"virtual-1", "DISPLAY15"});
  state.frame("virtual-1", old);
  EXPECT_FALSE(watch->frame_observed.load());
  const auto next = state.begin_capture();
  state.frame("physical", next);
  EXPECT_FALSE(watch->frame_observed.load());
  state.frame("display15", next);
  EXPECT_TRUE(watch->frame_observed.load());
}

TEST(CaptureRecovery, ConcurrentClientsAndRetiredObservationsStayIndependent) {
  video::capture_recovery::observations_t state;
  auto first = state.watch({"client1"});
  const auto second = state.watch({"client2"});
  first.reset();
  const auto next = state.begin_capture();
  state.frame("client1", next);
  EXPECT_FALSE(second->frame_observed.load());
  state.frame("client2", next);
  EXPECT_TRUE(second->frame_observed.load());
  const auto replacement = state.watch({"client2"});
  state.frame("client2", next);
  EXPECT_FALSE(replacement->frame_observed.load());
}

TEST(CaptureRecovery, ActiveTargetWithoutFramesRemainsPendingAfterBoundedRetries) {
  using namespace video::capture_recovery;
  using namespace std::chrono_literals;
  observations_t state;
  confirmation_t confirmation;
  const auto now = confirmation_t::clock::now();
  const auto watch = state.watch({"virtual1"});
  confirmation.start(watch, now);
  EXPECT_EQ(confirmation.poll(now + 4s), confirmation_status::waiting);
  EXPECT_EQ(confirmation.poll(now + 5s), confirmation_status::retry);
  EXPECT_EQ(confirmation.poll(now + 10s), confirmation_status::retry);
  EXPECT_EQ(confirmation.poll(now + 15s), confirmation_status::stalled);
  EXPECT_TRUE(confirmation.pending());
  EXPECT_EQ(confirmation.poll(now + 30s), confirmation_status::waiting);
  state.frame("virtual1", state.begin_capture());
  EXPECT_EQ(confirmation.poll(now + 31s), confirmation_status::confirmed);
  EXPECT_FALSE(confirmation.pending());
}

TEST(CaptureRecovery, CancellationDropsPendingObservationBeforeAnotherSession) {
  using namespace video::capture_recovery;
  observations_t state;
  confirmation_t confirmation;
  confirmation.start(state.watch({"virtual1"}), confirmation_t::clock::now());
  confirmation.reset();
  EXPECT_FALSE(confirmation.pending());
  const auto old_capture = state.begin_capture();
  confirmation.start(state.watch({"virtual1"}), confirmation_t::clock::now());
  state.frame("virtual1", old_capture);
  EXPECT_EQ(confirmation.poll(confirmation_t::clock::now()), confirmation_status::waiting);
}

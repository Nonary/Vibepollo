/**
 * @file tests/unit/test_display_helper_session_deferral.cpp
 * @brief Unit tests for Sunshine display helper session deferral.
 */
#include "../tests_common.h"

#include "src/platform/windows/display_helper_session_deferral.h"
#include "src/rtsp.h"

namespace {
  class FakeClock {
  public:
    std::chrono::steady_clock::time_point now() const {
      return now_;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  display_helper_integration::DisplayApplyRequest make_request(rtsp_stream::launch_session_t &session) {
    display_helper_integration::DisplayApplyRequest request;
    request.action = display_helper_integration::DisplayApplyAction::Apply;
    request.session = &session;
    request.configuration = display_device::SingleDisplayConfiguration {};
    return request;
  }
}  // namespace

TEST(DisplayHelperSessionDeferral, DelaysAndRestoresSessionSnapshot) {
  FakeClock clock;
  display_helper_integration::SessionDeferralManager manager([&clock]() { return clock.now(); });

  rtsp_stream::launch_session_t session {};
  session.id = 42;
  session.width = 1920;
  session.height = 1080;
  session.fps = 60;
  session.enable_hdr = true;
  session.enable_sops = true;
  session.virtual_display = true;
  session.virtual_display_device_id = "VD";
  session.framegen_refresh_rate = 120;
  session.gen1_framegen_fix = true;
  session.gen2_framegen_fix = false;

  manager.set_pending(make_request(session));

  auto result = manager.take_ready(false);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::SessionNotReady);

  result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(display_helper_integration::SessionDeferralManager::initial_delay() - std::chrono::milliseconds(1));
  result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayPending);

  clock.advance(std::chrono::milliseconds(1));
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  const auto &snapshot = result.pending->session_snapshot;
  EXPECT_EQ(snapshot.width, 1920);
  EXPECT_EQ(snapshot.height, 1080);
  EXPECT_EQ(snapshot.fps, 60);
  EXPECT_TRUE(snapshot.enable_hdr);
  EXPECT_TRUE(snapshot.enable_sops);
  EXPECT_TRUE(snapshot.virtual_display);
  EXPECT_EQ(snapshot.virtual_display_device_id, "VD");
  ASSERT_TRUE(snapshot.framegen_refresh_rate.has_value());
  EXPECT_EQ(*snapshot.framegen_refresh_rate, 120);
  EXPECT_TRUE(snapshot.gen1_framegen_fix);
  EXPECT_FALSE(snapshot.gen2_framegen_fix);
}

TEST(DisplayHelperSessionDeferral, ReschedulesAndDropsForNewerPending) {
  FakeClock clock;
  display_helper_integration::SessionDeferralManager manager([&clock]() { return clock.now(); });

  rtsp_stream::launch_session_t session {};
  session.id = 1;
  manager.set_pending(make_request(session));

  auto result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(display_helper_integration::SessionDeferralManager::initial_delay());
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  auto reschedule = manager.reschedule(*result.pending);
  EXPECT_TRUE(reschedule.requeued);
  EXPECT_EQ(reschedule.delay, display_helper_integration::SessionDeferralManager::retry_delay(1));

  clock.advance(reschedule.delay);
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  rtsp_stream::launch_session_t newer_session {};
  newer_session.id = 2;
  manager.set_pending(make_request(newer_session));

  reschedule = manager.reschedule(*result.pending);
  EXPECT_TRUE(reschedule.dropped_for_newer);
}

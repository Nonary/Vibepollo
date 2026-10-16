#include <gtest/gtest.h>
#include <src/platform/linux/display_restore_dispatcher.h>

#include <atomic>
#include <future>
#include <latch>

using dispatcher_t = platf::linux_private_display::restore_dispatcher_t;
using namespace std::chrono_literals;

TEST(LinuxDisplayRestoreDispatcher, DispatchReturnsWhileHelperIsBlockedAndSerializesNextRequest) {
  dispatcher_t dispatcher;
  std::latch entered {1}, release {1};
  std::promise<void> second_done;
  std::atomic_bool first_finished {false};
  ASSERT_TRUE(dispatcher.submit(1, [&](std::stop_token) {
    entered.count_down();
    release.wait();
    first_finished = true;
  }));
  entered.wait();
  auto dispatch = std::async(std::launch::async, [&] {
    return dispatcher.submit(2, [&](std::stop_token) {
      EXPECT_TRUE(first_finished);
      second_done.set_value();
    });
  });
  const auto status = dispatch.wait_for(200ms);
  release.count_down();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_TRUE(dispatch.get());
  EXPECT_EQ(second_done.get_future().wait_for(1s), std::future_status::ready);
}

TEST(LinuxDisplayRestoreDispatcher, NewAdmissionCancelsDelayedRestoreWithoutWaitingForDelay) {
  dispatcher_t dispatcher;
  std::atomic_uint calls {0};
  ASSERT_TRUE(dispatcher.submit(1, [&](std::stop_token) { ++calls; }, 24h));
  dispatcher.cancel(2);
  EXPECT_FALSE(dispatcher.submit(1, [&](std::stop_token) { ++calls; }));
  std::promise<void> done;
  ASSERT_TRUE(dispatcher.submit(3, [&](std::stop_token) { ++calls; done.set_value(); }));
  EXPECT_EQ(done.get_future().wait_for(1s), std::future_status::ready);
  EXPECT_EQ(calls, 1U);
}

TEST(LinuxDisplayRestoreDispatcher, LatestQueuedIntentSupersedesOldRestoreAndFailureDoesNotKillWorker) {
  dispatcher_t dispatcher;
  std::latch entered {1}, release {1};
  std::atomic_uint old_calls {0};
  std::promise<void> done;
  ASSERT_TRUE(dispatcher.submit(1, [&](std::stop_token) { entered.count_down(); release.wait(); }));
  entered.wait();
  EXPECT_TRUE(dispatcher.submit(2, [&](std::stop_token) { ++old_calls; }));
  EXPECT_TRUE(dispatcher.submit(3, [&](std::stop_token) { done.set_value(); throw std::runtime_error("helper failed"); }));
  release.count_down();
  EXPECT_EQ(done.get_future().wait_for(1s), std::future_status::ready);
  EXPECT_EQ(old_calls, 0U);
  std::promise<void> recovered;
  EXPECT_TRUE(dispatcher.submit(4, [&](std::stop_token) { recovered.set_value(); }));
  EXPECT_EQ(recovered.get_future().wait_for(1s), std::future_status::ready);
}

TEST(LinuxDisplayRestoreDispatcher, ShutdownCancelsLongDelayAndRejectsFurtherWork) {
  dispatcher_t dispatcher;
  std::atomic_uint calls {0};
  ASSERT_TRUE(dispatcher.submit(1, [&](std::stop_token) { ++calls; }, 24h));
  const auto start = std::chrono::steady_clock::now();
  dispatcher.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
  EXPECT_EQ(calls, 0U);
  EXPECT_FALSE(dispatcher.submit(2, [&](std::stop_token) { ++calls; }));
}

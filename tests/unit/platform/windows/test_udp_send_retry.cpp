#include <gtest/gtest.h>

#include "src/platform/windows/udp_send_retry.h"
#include "src/video_send.h"

#include <algorithm>
#include <vector>

namespace {
  namespace retry = platf::udp_send_retry;
  using namespace std::chrono_literals;

  struct sender_t {
    std::chrono::steady_clock::time_point time {};
    std::vector<int> errors;
    unsigned calls = 0;
    unsigned waits = 0;
    bool cancelled = false;
    int wait_error = 0;
    bool cancel_during_wait = false;

    retry::result_t send() {
      return retry::send(
        [&]() { return errors.at(calls++); },
        [&](int error, auto remaining) {
          EXPECT_TRUE(retry::pressure_error(error));
          EXPECT_GT(remaining, 0ns);
          EXPECT_LE(remaining, retry::retry_budget);
          ++waits;
          time += std::min(remaining, std::chrono::steady_clock::duration {1ms});
          cancelled |= cancel_during_wait;
          return wait_error;
        },
        [&]() { return cancelled; },
        [&]() { return time; }
      );
    }
  };

  struct video_sender_t {
    stream::video_send_fallback_t fallback;
    std::chrono::steady_clock::time_point now {};
    unsigned segmented_attempts = 0;
    unsigned individual_attempts = 0;
    unsigned waits = 0;
    bool recovering = false;

    stream::video_batch_send_result_t frame(int segmented_error, std::vector<int> individual_errors = {},
                                           bool cancel_after_first_datagram = false) {
      const bool individual = fallback.send_individually(now);
      const auto cancelled = [&] { return cancel_after_first_datagram && individual_attempts != 0; };
      bool segmented_would_block = false;
      bool allow_fallback = false;
      const auto result = stream::send_video_batch(
        3,
        allow_fallback,
        [&] {
          const auto batch = retry::send(
            [&] { ++segmented_attempts; return segmented_error; },
            [&](int, auto remaining) {
              ++waits;
              now += std::min(remaining, std::chrono::steady_clock::duration {1ms});
              return 0;
            },
            cancelled,
            [&] { return now; }
          );
          segmented_would_block = retry::segmented_send_would_block(batch, true);
          return batch.error == 0;
        },
        [&](std::size_t packet) {
          return retry::send(
            [&] { ++individual_attempts; return individual_errors.at(packet); },
            [&](int, auto) { ++waits; return 0; },
            cancelled,
            [&] { return now; },
            0ms
          ).error == 0;
        },
        cancelled,
        individual
      );
      if (result.sent) {
        recovering = fallback.frame_sent(individual, now);
      } else if (!result.cancelled) {
        fallback.frame_failed(individual, segmented_would_block, now);
      }
      return result;
    }
  };
}  // namespace

TEST(WindowsUdpSendRetry, TemporaryPressureRetainsTheMessageUntilAccepted) {
  for (const auto error : {WSAENOBUFS, WSAEWOULDBLOCK}) {
    sender_t sender {.errors = {error, 0}};
    const auto result = sender.send();
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.retries, 1u);
    EXPECT_EQ(sender.calls, 2u);
    EXPECT_EQ(sender.waits, 1u);
    EXPECT_FALSE(result.timed_out);
  }
}

TEST(WindowsUdpSendRetry, PersistentPressureStopsAtTheBudgetWithoutAnotherSend) {
  sender_t sender {.errors = {WSAENOBUFS, WSAENOBUFS}};
  const auto result = sender.send();
  EXPECT_EQ(result.error, WSAENOBUFS);
  EXPECT_TRUE(result.timed_out);
  EXPECT_EQ(sender.time.time_since_epoch(), retry::retry_budget);
  EXPECT_EQ(sender.calls, 2u);
}

TEST(WindowsUdpSendRetry, CancellationBeforeSendingAndWhileWaitingStopsFurtherAttempts) {
  sender_t already_stopped {.errors = {}, .cancelled = true};
  EXPECT_TRUE(already_stopped.send().cancelled);
  EXPECT_EQ(already_stopped.calls, 0u);

  sender_t stopped_during_wait {.errors = {WSAEWOULDBLOCK}, .cancel_during_wait = true};
  const auto result = stopped_during_wait.send();
  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.error, WSAEINTR);
  EXPECT_EQ(stopped_during_wait.calls, 1u);
}

TEST(WindowsUdpSendRetry, PermanentSendAndSocketWaitErrorsAreNotRetried) {
  sender_t unsupported {.errors = {WSAEOPNOTSUPP}};
  EXPECT_EQ(unsupported.send().error, WSAEOPNOTSUPP);
  EXPECT_EQ(unsupported.waits, 0u);

  sender_t closed_socket {.errors = {WSAEWOULDBLOCK}, .wait_error = WSAENOTSOCK};
  EXPECT_EQ(closed_socket.send().error, WSAENOTSOCK);
  EXPECT_EQ(closed_socket.calls, 1u);
}

TEST(WindowsUdpSendRetry, FallbackIsLimitedToRejectedOffloadWithValidIndividualDatagrams) {
  for (const auto error : {WSAEINVAL, WSAEOPNOTSUPP, WSAENOPROTOOPT, WSAEMSGSIZE}) {
    EXPECT_TRUE(retry::offload_fallback_error(error, true, 1408, false));
    EXPECT_FALSE(retry::offload_fallback_error(error, false, 1408, false));
  }
  for (const auto error : {WSAENOBUFS, WSAEWOULDBLOCK, WSAENOTSOCK, WSAECONNRESET, WSAEINTR}) {
    EXPECT_FALSE(retry::offload_fallback_error(error, true, 1408, false));
  }
  EXPECT_FALSE(retry::offload_fallback_error(WSAEMSGSIZE, true, 65508, false));
  EXPECT_TRUE(retry::offload_fallback_error(WSAEMSGSIZE, true, 65508, true));
  EXPECT_FALSE(retry::offload_fallback_error(WSAEMSGSIZE, true, 65528, true));
}

TEST(WindowsUdpSendRetry, VideoRecoversWhenOnlySegmentedSendsWouldBlock) {
  video_sender_t sender;
  for (int frame = 0; frame < 2; ++frame) {
    const auto blocked = sender.frame(WSAEWOULDBLOCK);
    EXPECT_FALSE(blocked.sent);
    EXPECT_EQ(blocked.packets_accepted, 0u);
    EXPECT_EQ(sender.individual_attempts, 0u);
  }

  const auto trial = sender.frame(WSAEWOULDBLOCK, {0, 0, 0});
  EXPECT_TRUE(trial.sent);
  EXPECT_EQ(trial.packets_accepted, 3u);
  EXPECT_TRUE(sender.recovering);
  EXPECT_EQ(sender.segmented_attempts, 4u);
  EXPECT_EQ(sender.individual_attempts, 3u);
  EXPECT_EQ(sender.waits, 4u);

  EXPECT_TRUE(sender.frame(WSAEWOULDBLOCK, {0, 0, 0}).sent);
  EXPECT_FALSE(sender.recovering);
  sender.now += 5s;
  EXPECT_TRUE(sender.frame(0).sent);
  EXPECT_EQ(sender.segmented_attempts, 5u);
  EXPECT_EQ(sender.individual_attempts, 6u);
}

TEST(WindowsUdpSendRetry, VideoStopsWithoutWaitingWhenBothSendModesWouldBlock) {
  video_sender_t sender;
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  const auto trial = sender.frame(WSAEWOULDBLOCK, {WSAEWOULDBLOCK});
  EXPECT_FALSE(trial.sent);
  EXPECT_EQ(trial.packets_accepted, 0u);
  EXPECT_FALSE(sender.recovering);
  EXPECT_EQ(sender.individual_attempts, 1u);
  EXPECT_EQ(sender.waits, 4u);

  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.fallback.send_individually(sender.now));
  EXPECT_EQ(sender.individual_attempts, 1u);

  sender.now += 1s;
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_TRUE(sender.frame(WSAEWOULDBLOCK, {0, 0, 0}).sent);
}

TEST(WindowsUdpSendRetry, VideoRecoveryCountsTheAcceptedPrefixAndStops) {
  video_sender_t sender;
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  const auto trial = sender.frame(WSAEWOULDBLOCK, {0, WSAEWOULDBLOCK});
  EXPECT_FALSE(trial.sent);
  EXPECT_EQ(trial.packets_accepted, 1u);
  EXPECT_EQ(sender.individual_attempts, 2u);
  EXPECT_EQ(sender.waits, 4u);
  EXPECT_FALSE(sender.fallback.send_individually(sender.now));
}

TEST(WindowsUdpSendRetry, CancellingVideoRecoveryStopsBeforeTheNextDatagram) {
  video_sender_t sender;
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  const auto trial = sender.frame(WSAEWOULDBLOCK, {0}, true);
  EXPECT_FALSE(trial.sent);
  EXPECT_TRUE(trial.cancelled);
  EXPECT_EQ(trial.packets_accepted, 1u);
  EXPECT_EQ(sender.individual_attempts, 1u);
  EXPECT_EQ(sender.waits, 4u);
}

TEST(WindowsUdpSendRetry, VideoProbesOnlyAfterConsecutiveSegmentedWouldBlockFailures) {
  video_sender_t sender;
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_TRUE(sender.frame(0).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.frame(WSAENOBUFS).sent);
  EXPECT_FALSE(sender.frame(WSAEWOULDBLOCK).sent);
  EXPECT_FALSE(sender.fallback.send_individually(sender.now));
  EXPECT_FALSE(retry::segmented_send_would_block({.error = WSAEWOULDBLOCK, .timed_out = true}, false));
  EXPECT_FALSE(retry::segmented_send_would_block({.error = WSAEWOULDBLOCK}, true));
  EXPECT_EQ(sender.individual_attempts, 0u);
}

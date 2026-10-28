#include <gtest/gtest.h>

#include "src/platform/windows/udp_send_retry.h"

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

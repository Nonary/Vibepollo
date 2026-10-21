#include "src/host_stream_control.h"
#include "src/platform/windows/end_stream_hotkey_protocol.h"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace std::chrono_literals;
using namespace platf::hotkey::end_stream;

TEST(EndStreamHotkey, RequiresAnExplicitSupportedBinding) {
  EXPECT_FALSE(valid_binding(0, 7));
  EXPECT_FALSE(valid_binding('Q', 0));
  EXPECT_FALSE(valid_binding('Q', 0x4007));
  EXPECT_FALSE(valid_binding(0x1B, 7));
  EXPECT_TRUE(valid_binding('Q', 7));
  EXPECT_TRUE(valid_binding('0', 1));
  EXPECT_TRUE(valid_binding(0x70, 2));
  EXPECT_TRUE(valid_binding(0x87, 8));
  EXPECT_FALSE(valid_binding(0x88, 7));
}

TEST(EndStreamHotkey, RejectsOtherProcessesAndStaleConsoleSessions) {
  EXPECT_TRUE(authorized_sender(123, 123, 2, 2, 2));
  EXPECT_FALSE(authorized_sender(123, 124, 2, 2, 2));
  EXPECT_FALSE(authorized_sender(123, 123, 2, 3, 2));
  EXPECT_FALSE(authorized_sender(123, 123, 2, 2, 3));
  EXPECT_FALSE(authorized_sender(123, 123, 0, 0, 0));
  EXPECT_FALSE(authorized_sender(123, 123, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF));
  EXPECT_FALSE(authorized_sender(0, 0, 2, 2, 2));
}

TEST(HostStreamControl, DrainsAdmittedStartsBeforeTermination) {
  host_stream_control::start_guard_t in_flight {host_stream_control::request_ticket()};
  std::promise<void> attempted;
  auto attempt = attempted.get_future();
  auto terminate = std::async(std::launch::async, [&] {
    attempted.set_value();
    EXPECT_TRUE(host_stream_control::terminate([] { return true; }, [] {}));
  });
  ASSERT_EQ(attempt.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(terminate.wait_for(50ms), std::future_status::timeout);
  in_flight.unlock();
  EXPECT_EQ(terminate.wait_for(2s), std::future_status::ready);
  terminate.get();
}

TEST(HostStreamControl, RejectsNewActivityDuringCleanupAndReopens) {
  std::promise<void> entered, release;
  auto may_finish = release.get_future();
  auto terminal = std::async(std::launch::async, [&] {
    return host_stream_control::terminate([] { return true; }, [&] {
      host_stream_control::activity_guard_t nested;
      EXPECT_TRUE(nested);
      entered.set_value();
      may_finish.wait();
    });
  });
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  {
    host_stream_control::activity_guard_t cleanup;
    EXPECT_FALSE(cleanup);
    host_stream_control::start_guard_t start {host_stream_control::request_ticket()};
    EXPECT_FALSE(start);
  }
  release.set_value();
  EXPECT_TRUE(terminal.get());
  host_stream_control::activity_guard_t fresh;
  EXPECT_TRUE(fresh);
}

TEST(HostStreamControl, DrainTimeoutDoesNotRunRecoveryAndReopens) {
  host_stream_control::activity_guard_t ongoing;
  bool recovered = false;
  auto terminal = std::async(std::launch::async, [&] {
    return host_stream_control::terminate([] { return true; }, [&] { recovered = true; }, 30ms);
  });
  EXPECT_FALSE(terminal.get());
  EXPECT_FALSE(recovered);
  ongoing.unlock();
  host_stream_control::activity_guard_t fresh;
  EXPECT_TRUE(fresh);
}

TEST(HostStreamControl, RevalidatesOwnershipAfterDrainingStarts) {
  host_stream_control::start_guard_t in_flight {host_stream_control::request_ticket()};
  std::atomic_bool authorized {true};
  std::atomic_bool executed {false};
  std::promise<void> attempted;
  auto attempt = attempted.get_future();
  auto terminate = std::async(std::launch::async, [&] {
    attempted.set_value();
    return host_stream_control::terminate([&] {
      return authorized.load();
    },
                                          [&] {
                                            executed = true;
                                          });
  });
  ASSERT_EQ(attempt.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(terminate.wait_for(50ms), std::future_status::timeout);
  // The console user changed or the shortcut was disabled while we waited.
  authorized = false;
  in_flight.unlock();
  ASSERT_EQ(terminate.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(terminate.get());
  EXPECT_FALSE(executed.load());
  EXPECT_TRUE(host_stream_control::terminate([] {
    return true;
  },
                                             [&] {
                                               executed = true;
                                             }));
  EXPECT_TRUE(executed.load());
}

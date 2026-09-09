/**
 * @file tests/unit/platform/linux/test_private_display_cleanup_policy.cpp
 * @brief Deterministic delayed-display cleanup admission races.
 */
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <mutex>
#include <src/platform/linux/private_display_cleanup_policy.h>
#include <string>

namespace policy = platf::linux_private_display::cleanup_policy;

TEST(LinuxPrivateDisplayCleanupPolicy, NewAdmissionSupersedesWorkerWaitingForLifecycleGate) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  std::string output = "old game";
  std::latch worker_started {1};
  std::unique_lock admission {lifecycle};
  auto worker = std::async(std::launch::async, [&] {
    worker_started.count_down();
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [] { return false; }, [&] { output.clear(); return true; });
  });
  worker_started.wait();
  {
    std::lock_guard reservation {display};
    generation.fetch_add(1);
    output = "new monitor";
  }
  admission.unlock();
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(output, "new monitor");
}

TEST(LinuxPrivateDisplayCleanupPolicy, CancellationBetweenOwnershipCheckAndDisplayLockPreservesNewReservation) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  std::string output = "old game";
  std::latch ownership_checked {1};
  std::latch reservation_published {1};
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [&] {
        ownership_checked.count_down();
        reservation_published.wait();
        return false;
      },
      [&] { output.clear(); return true; });
  });
  ownership_checked.wait();
  {
    std::lock_guard reservation {display};
    generation.fetch_add(1);
    output = "new monitor";
  }
  reservation_published.count_down();
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(output, "new monitor");
}

TEST(LinuxPrivateDisplayCleanupPolicy, RetainedMonitorAndCaptureActivityPreventRestore) {
  for (const bool capture_active : {false, true}) {
    for (const bool retained_monitor : {false, true}) {
      std::mutex lifecycle;
      std::mutex display;
      std::atomic<std::uint64_t> generation {1};
      bool restored = false;
      const auto result = policy::run_delayed_restore(lifecycle, display, generation, 1,
        [&] { return capture_active || retained_monitor; },
        [&] { restored = true; return true; });
      EXPECT_EQ(restored, !capture_active && !retained_monitor);
      EXPECT_EQ(result, restored ? policy::result_e::restored : policy::result_e::owned);
    }
  }
}

TEST(LinuxPrivateDisplayCleanupPolicy, PausedTimeoutWithNoCaptureOrMonitorRestoresOnce) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  int restores = 0;
  auto restore = [&] { ++restores; return true; };
  EXPECT_EQ(policy::run_delayed_restore(lifecycle, display, generation, 1,
              [] { return false; }, restore), policy::result_e::restored);
  EXPECT_EQ(policy::run_delayed_restore(lifecycle, display, generation, 1,
              [] { return false; }, restore), policy::result_e::superseded);
  EXPECT_EQ(restores, 1);
}

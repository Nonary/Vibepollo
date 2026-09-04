#include "../../../tests_common.h"
#include <src/platform/linux/capture_status.h>
#include <thread>
#include <vector>

namespace status = platf::linux_capture_status;

TEST(LinuxCaptureStatus, ActiveOnlyWithinCaptureLifetime) {
  EXPECT_FALSE(status::managed_event_capture_active());
  {
    const status::managed_capture_scope capture;
    EXPECT_TRUE(status::managed_event_capture_active());
    {
      const status::managed_capture_scope second;
    }
    EXPECT_TRUE(status::managed_event_capture_active());
  }
  EXPECT_FALSE(status::managed_event_capture_active());
}

TEST(LinuxCaptureStatus, ConcurrentCaptureCleanupPreservesOtherStreams) {
  {
    const status::managed_capture_scope retained;
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([] {
        for (int n = 0; n < 100; ++n) {
          const status::managed_capture_scope capture;
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    EXPECT_TRUE(status::managed_event_capture_active());
  }
  EXPECT_FALSE(status::managed_event_capture_active());
}

TEST(LinuxCaptureStatus, FailureUnwindsActiveStatus) {
  try {
    const status::managed_capture_scope capture;
    throw 1;
  } catch (...) {}
  EXPECT_FALSE(status::managed_event_capture_active());
}

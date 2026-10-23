#include "../../../tests_common.h"
#include "src/platform/windows/gpu_priority.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace {
  struct driver_t {
    int priority = 2;
    bool query_ok = true;
    bool set_ok = true;
    std::vector<int> writes;
    platf::dxgi::gpu_priority_manager_t manager {
      [this]() -> std::optional<int> {
        return query_ok ? std::optional<int>(priority) : std::nullopt;
      },
      [this](int value) {
        writes.push_back(value);
        if (!set_ok) {
          return false;
        }
        priority = value;
        return true;
      }
    };
  };
}  // namespace

TEST(GpuPriorityLifetime, RestoresOriginalAfterLastCaptureNotFirst) {
  driver_t driver;
  driver.priority = 1;
  auto first = driver.manager.acquire(5);
  auto second = driver.manager.acquire(4);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  first.reset();
  EXPECT_EQ(driver.priority, 4);
  second.reset();
  EXPECT_EQ(driver.priority, 1);
  EXPECT_EQ(driver.writes, (std::vector<int> {5, 4, 1}));
}

TEST(GpuPriorityLifetime, FailedInitializationAndExceptionReleaseTheirLease) {
  driver_t driver;
  auto initialize = [&]() {
    auto capture = driver.manager.acquire(5);
    EXPECT_TRUE(capture);
    return false;
  };
  EXPECT_FALSE(initialize());
  EXPECT_EQ(driver.priority, 2);
  try {
    auto capture = driver.manager.acquire(5);
    throw 1;
  } catch (int) {
  }
  EXPECT_EQ(driver.priority, 2);
}

TEST(GpuPriorityLifetime, CannotBoostWithoutRestorableBaseline) {
  driver_t driver;
  driver.query_ok = false;
  EXPECT_FALSE(driver.manager.acquire(5));
  EXPECT_TRUE(driver.writes.empty());
  driver.query_ok = true;
  driver.set_ok = false;
  EXPECT_FALSE(driver.manager.acquire(5));
  driver.set_ok = true;
  auto capture = driver.manager.acquire(4);
  ASSERT_TRUE(capture);
  capture.reset();
  EXPECT_EQ(driver.priority, 2);
}

TEST(GpuPriorityLifetime, FailedOverlappingBoostDoesNotReleaseActiveOwnership) {
  driver_t driver;
  auto first = driver.manager.acquire(5);
  driver.set_ok = false;
  EXPECT_FALSE(driver.manager.acquire(4));
  EXPECT_EQ(driver.priority, 5);
  driver.set_ok = true;
  first.reset();
  EXPECT_EQ(driver.priority, 2);
}

TEST(GpuPriorityLifetime, PreservesExternalChangesBetweenCaptureAndTeardown) {
  driver_t driver;
  auto capture = driver.manager.acquire(5);
  driver.priority = 1;
  capture.reset();
  EXPECT_EQ(driver.priority, 1);
  auto next = driver.manager.acquire(4);
  next.reset();
  EXPECT_EQ(driver.priority, 1);
}

TEST(GpuPriorityLifetime, LaterCaptureAdoptsAnExternallyChangedBaseline) {
  driver_t driver;
  auto first = driver.manager.acquire(5);
  driver.priority = 1;
  auto second = driver.manager.acquire(4);
  first.reset();
  EXPECT_EQ(driver.priority, 4);
  second.reset();
  EXPECT_EQ(driver.priority, 1);
}

TEST(GpuPriorityLifetime, FailedRestorationKeepsOriginalForNextCapture) {
  driver_t driver;
  auto capture = driver.manager.acquire(5);
  driver.set_ok = false;
  capture.reset();
  EXPECT_EQ(driver.priority, 5);
  driver.set_ok = true;
  auto next = driver.manager.acquire(4);
  next.reset();
  EXPECT_EQ(driver.priority, 2);
}

TEST(GpuPriorityLifetime, FailedRestoreQueryIsRetriedAndExternalIdleChangeRespected) {
  driver_t driver;
  auto capture = driver.manager.acquire(5);
  driver.query_ok = false;
  capture.reset();
  EXPECT_FALSE(driver.manager.acquire(4));
  EXPECT_EQ(driver.priority, 5);
  driver.query_ok = true;
  driver.priority = 1;
  auto next = driver.manager.acquire(4);
  next.reset();
  EXPECT_EQ(driver.priority, 1);
}

TEST(GpuPriorityLifetime, LeasesKeepRestorationStateAliveBeyondManager) {
  int priority = 2;
  std::unique_ptr<platf::dxgi::gpu_priority_manager_t::lease_t> capture;
  {
    platf::dxgi::gpu_priority_manager_t manager {
      [&]() -> std::optional<int> {
        return priority;
      },
      [&](int value) {
        priority = value;
        return true;
      }
    };
    capture = manager.acquire(5);
  }
  EXPECT_EQ(priority, 5);
  capture.reset();
  EXPECT_EQ(priority, 2);
}

TEST(GpuPriorityLifetime, ConcurrentCapturesRestoreOnlyAfterFinalOwner) {
  driver_t driver;
  auto outer = driver.manager.acquire(5);
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&]() {
      for (int n = 0; n < 100; ++n) {
        auto capture = driver.manager.acquire(4);
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  EXPECT_EQ(driver.priority, 4);
  outer.reset();
  EXPECT_EQ(driver.priority, 2);
  EXPECT_EQ(driver.writes.back(), 2);
  EXPECT_EQ(std::count(driver.writes.begin(), driver.writes.end(), 2), 1);
}

/**
 * @file tests/unit/test_paused_app_monitor.cpp
 * @brief Paused observation, serialization and worker lifetime regressions.
 */
#include "src/paused_app_monitor.h"
#include "src/power_action.h"

#include <array>

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace {
  using namespace std::chrono_literals;

  // The same serialized observer used by proc_t, with process and capture
  // edges substituted so these tests need neither a game nor a display driver.
  struct application_t {
    std::mutex lifecycle;
    std::atomic_int id {1};
    bool deferred = false;
    bool capture_owned = false;
    bool alive = true;
    int polls = 0;
    int cleanups = 0;

    int observe() {
      return proc::observe_app_liveness(
        lifecycle,
        [&] { return id.load(); },
        [&] { return proc::paused_app_poll_allowed(id, deferred, capture_owned); },
        [&] {
          ++polls;
          if (!alive) {
            ++cleanups;
            id = -1;
          }
          return id.load();
        }
      );
    }
  };
}

TEST(PausedAppMonitor, RunningPausedGameIsRetainedAndLaterExitCleansUpOnce) {
  application_t app;
  EXPECT_EQ(app.observe(), 1);
  EXPECT_EQ(app.cleanups, 0);
  app.alive = false;
  EXPECT_EQ(app.observe(), -1);
  EXPECT_EQ(app.observe(), -1);
  EXPECT_EQ(app.cleanups, 1);
  EXPECT_EQ(app.polls, 2);
}

TEST(PausedAppMonitor, DeferredLaunchIsNeverAdvancedWithoutAClient) {
  application_t app;
  app.deferred = true;
  for (int i = 0; i < 10; ++i) EXPECT_EQ(app.observe(), 1);
  EXPECT_EQ(app.polls, 0);
  EXPECT_EQ(app.cleanups, 0);
  app.deferred = false;
  EXPECT_EQ(app.observe(), 1);
  EXPECT_EQ(app.polls, 1);
}

TEST(PausedAppMonitor, LivePendingOrDrainingCaptureSkipsBackgroundObservation) {
  application_t app;
  app.capture_owned = true;
  app.alive = false;
  EXPECT_EQ(app.observe(), 1);
  EXPECT_EQ(app.polls, 0);
  app.capture_owned = false;
  EXPECT_EQ(app.observe(), -1);
  EXPECT_EQ(app.cleanups, 1);
}

TEST(PausedAppMonitor, BusyLifecycleReturnsSnapshotWithoutAccessingProcessState) {
  application_t app;
  std::unique_lock owner(app.lifecycle);
  auto observer = std::async(std::launch::async, [&] { return app.observe(); });
  EXPECT_EQ(observer.wait_for(5s), std::future_status::ready);
  owner.unlock();
  EXPECT_EQ(observer.get(), 1);
  EXPECT_EQ(app.polls, 0);
}

TEST(PausedAppMonitor, ReplacementAndDeferredEligibilityAreObservedUnderOneGate) {
  application_t app;
  app.alive = false;
  std::unique_lock owner(app.lifecycle);
  auto observer = std::async(std::launch::async, [&] { return app.observe(); });
  EXPECT_EQ(observer.wait_for(5s), std::future_status::ready);
  // An exited old app is replaced by a new deferred app under the gate.
  app.id = 2;
  app.alive = true;
  app.deferred = true;
  owner.unlock();
  observer.get();
  EXPECT_EQ(app.observe(), 2);
  EXPECT_EQ(app.polls, 0);
  EXPECT_EQ(app.cleanups, 0);
}

TEST(PausedAppMonitor, ParallelObserversNeverOverlapOrDuplicateCleanup) {
  application_t app;
  app.alive = false;
  std::vector<std::thread> observers;
  for (int i = 0; i < 32; ++i) {
    observers.emplace_back([&] {
      for (int j = 0; j < 100; ++j) app.observe();
    });
  }
  for (auto &thread : observers) thread.join();
  EXPECT_EQ(app.cleanups, 1);
  EXPECT_EQ(app.polls, 1);
}

TEST(PausedAppMonitor, ExceptionReleasesObservationGateForTheNextPoll) {
  std::mutex gate;
  EXPECT_THROW(proc::observe_app_liveness(gate, [] { return 1; }, [] { return true; }, []() -> int {
    throw std::runtime_error("process query failed");
  }), std::runtime_error);
  EXPECT_EQ(proc::observe_app_liveness(gate, [] { return 1; }, [] { return true; }, [] { return 2; }), 2);
}

TEST(PausedAppMonitor, WorkerContinuesAfterIdleToObserveALaterApp) {
  application_t app;
  app.id = -1;
  std::promise<void> idle, cleaned;
  auto idle_ready = idle.get_future();
  auto cleaned_ready = cleaned.get_future();
  std::atomic_bool idle_signaled = false, cleaned_signaled = false;
  proc::paused_app_monitor_t monitor([&] {
    app.observe();
    // Inspect fixture state under the same gate as replacement below.
    std::lock_guard lock(app.lifecycle);
    if (!idle_signaled.exchange(true)) idle.set_value();
    if (app.cleanups == 1 && !cleaned_signaled.exchange(true)) cleaned.set_value();
  }, 1ms);
  EXPECT_EQ(idle_ready.wait_for(5s), std::future_status::ready);
  {
    std::lock_guard lock(app.lifecycle);
    app.id = 2;
    app.alive = false;
  }
  EXPECT_EQ(cleaned_ready.wait_for(5s), std::future_status::ready);
  monitor.stop();
  EXPECT_EQ(app.cleanups, 1);
}

TEST(PausedAppMonitor, StopInterruptsLongWaitAndIsIdempotent) {
  std::atomic_int polls = 0;
  proc::paused_app_monitor_t monitor([&] { ++polls; }, 1h);
  auto stopped = std::async(std::launch::async, [&] { monitor.stop(); });
  EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
  stopped.get();
  monitor.stop();
  EXPECT_EQ(polls, 0);
}

TEST(PausedAppMonitor, StopJoinsInFlightPollBeforeResourcesCanBeDestroyed) {
  std::promise<void> entered, release;
  auto entered_ready = entered.get_future();
  auto release_ready = release.get_future();
  std::atomic_int polls = 0;
  proc::paused_app_monitor_t monitor([&] {
    if (++polls == 1) entered.set_value();
    release_ready.wait();
  }, 1ms);
  EXPECT_EQ(entered_ready.wait_for(5s), std::future_status::ready);
  auto stopped = std::async(std::launch::async, [&] { monitor.stop(); });
  EXPECT_EQ(stopped.wait_for(20ms), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
  stopped.get();
  EXPECT_EQ(polls, 1);
}

TEST(PausedAppMonitor, SleepAdmissionRetainsAppAndIndependentCaptureOrLeaseOwnership) {
  namespace power = nvhttp::power_action;
  application_t app;
  app.id = -1;
  power::activity_t transports;
  int suspends = 0;
  int status = 0;
  power::service_t service(app.lifecycle);
  const power::hooks_t hooks {
    .authority = [] { return power::authority_t {"paired-client", true}; },
    .supported = [] { return true; },
    .activity = [&] {
      auto activity = transports;
      // Matches the NVHTTP hook: logical app ownership, not a recursive
      // liveness poll, while the power service owns the lifecycle gate.
      activity.app = app.id.load() > 0;
      return activity;
    },
    .suspend_if_authorized = [&] { ++suspends; return true; },
    .respond = [&](const power::response_t &response) { status = response.status; return true; },
  };
  const auto now = power::service_t::clock::now();
  const auto capability = service.capabilities(hooks, now);
  ASSERT_TRUE(capability.available);
  const power::request_t request {"POST", capability.token, true};

  app.id = 1;
  app.alive = false;
  EXPECT_FALSE(service.capabilities(hooks, now).available);
  EXPECT_EQ(service.sleep(request, hooks, now), power::result_e::refused);
  EXPECT_EQ(status, 409);
  EXPECT_EQ(app.polls, 0);
  EXPECT_EQ(suspends, 0);

  EXPECT_EQ(app.observe(), -1);
  EXPECT_EQ(app.cleanups, 1);
  const std::array independent_owners {
    &power::activity_t::pending, &power::activity_t::rtsp,
    &power::activity_t::running, &power::activity_t::teardown,
    &power::activity_t::webrtc, &power::activity_t::capture,
    &power::activity_t::remote_owner,
  };
  for (auto field : independent_owners) {
    transports = {};
    transports.*field = true;
    EXPECT_FALSE(service.capabilities(hooks, now).available);
    EXPECT_EQ(service.sleep(request, hooks, now), power::result_e::refused);
    EXPECT_EQ(status, 409);
  }
  EXPECT_EQ(suspends, 0);
  transports = {};
  EXPECT_EQ(service.sleep(request, hooks, now), power::result_e::requested);
  EXPECT_EQ(status, 202);
  EXPECT_EQ(suspends, 1);
}

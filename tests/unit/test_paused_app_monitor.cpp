/**
 * @file tests/unit/test_paused_app_monitor.cpp
 * @brief Lifecycle tests for the worker that watches a paused app.
 */
#include "src/paused_app_monitor.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

namespace {
  using namespace std::chrono_literals;

  bool eventually(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return condition();
  }

  struct fake_app_t {
    std::atomic_bool active {true};
    std::atomic_bool pollable {true};
    std::atomic_int polls {0};
    std::atomic_int concurrent_polls {0};
    std::atomic_bool overlapped {false};
    std::function<void()> on_poll;

    proc::paused_app_monitor_t::hooks_t hooks() {
      return {
        .app_active = [this] {
          return active.load();
        },
        .should_poll = [this] {
          return pollable.load();
        },
        .poll = [this] {
          if (concurrent_polls.fetch_add(1) != 0) {
            overlapped = true;
          }
          ++polls;
          if (on_poll) {
            on_poll();
          }
          concurrent_polls.fetch_sub(1);
        },
      };
    }
  };
}  // namespace

TEST(PausedAppMonitor, PollsUntilTheAppExitsThenStopsWatching) {
  // Mirrors the incident: the app exits while paused and only a poll can
  // run the cleanup that restores the physical displays.
  fake_app_t app;
  app.on_poll = [&app] {
    if (app.polls.load() >= 3) {
      app.active = false;
    }
  };
  proc::paused_app_monitor_t monitor {app.hooks(), 1ms};

  monitor.start();

  ASSERT_TRUE(eventually([&] {
    return !monitor.watching();
  }));
  EXPECT_EQ(app.polls.load(), 3);
}

TEST(PausedAppMonitor, SkippedPollsKeepWatching) {
  fake_app_t app;
  app.pollable = false;
  proc::paused_app_monitor_t monitor {app.hooks(), 1ms};

  monitor.start();
  std::this_thread::sleep_for(20ms);

  EXPECT_TRUE(monitor.watching());
  EXPECT_EQ(app.polls.load(), 0);

  app.pollable = true;
  EXPECT_TRUE(eventually([&] {
    return app.polls.load() > 0;
  }));
}

TEST(PausedAppMonitor, RepeatedStartsShareOneWorker) {
  fake_app_t app;
  app.on_poll = [] {
    std::this_thread::sleep_for(1ms);
  };
  proc::paused_app_monitor_t monitor {app.hooks(), 1ms};

  for (int i = 0; i < 50; ++i) {
    monitor.start();
  }
  ASSERT_TRUE(eventually([&] {
    return app.polls.load() >= 10;
  }));
  monitor.stop();

  EXPECT_FALSE(app.overlapped.load());
}

TEST(PausedAppMonitor, StartsAgainAfterAPreviousAppExited) {
  fake_app_t app;
  app.active = false;
  proc::paused_app_monitor_t monitor {app.hooks(), 1ms};

  monitor.start();
  ASSERT_TRUE(eventually([&] {
    return !monitor.watching();
  }));
  EXPECT_EQ(app.polls.load(), 0);

  // A later app is launched and paused.
  app.active = true;
  monitor.start();

  EXPECT_TRUE(monitor.watching());
  EXPECT_TRUE(eventually([&] {
    return app.polls.load() > 0;
  }));
}

TEST(PausedAppMonitor, StopInterruptsALongInterval) {
  fake_app_t app;
  proc::paused_app_monitor_t monitor {app.hooks(), 1h};

  monitor.start();
  const auto started = std::chrono::steady_clock::now();
  monitor.stop();

  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_FALSE(monitor.watching());
  EXPECT_EQ(app.polls.load(), 0);
}

TEST(PausedAppMonitor, PollMayReenterStartAndStop) {
  // proc_t::running() can terminate the app, and later lifecycle code may
  // pause or shut down from that same thread.
  fake_app_t app;
  proc::paused_app_monitor_t *monitor_ptr = nullptr;
  app.on_poll = [&] {
    monitor_ptr->start();
    monitor_ptr->stop();
    app.active = false;
  };
  proc::paused_app_monitor_t monitor {app.hooks(), 1ms};
  monitor_ptr = &monitor;

  monitor.start();

  ASSERT_TRUE(eventually([&] {
    return !monitor.watching();
  }));
  EXPECT_EQ(app.polls.load(), 1);
}

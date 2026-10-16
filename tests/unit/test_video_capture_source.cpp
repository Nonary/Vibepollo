#include "../tests_common.h"
#include "src/video_capture_source.h"

#include <future>
#include <memory>

namespace {
  struct capture_context_t {
    int generation = 0;
    safe::signal_t reinit;
  };

  using lease_t = video::capture_source_lease_t<capture_context_t>;
  using source_e = video::capture_source_e;

  // Like the production producer, this object has static storage. Refcounted
  // startup and teardown are exercised through the same lease used by video.cpp.
  std::atomic<int> starts {0};
  std::atomic<int> stops {0};
  std::function<void()> stop_hook;

  int start(capture_context_t &ctx) {
    ctx.generation = ++starts;
    return 0;
  }

  void stop(capture_context_t &) {
    if (stop_hook) {
      stop_hook();
    }
    ++stops;
  }

  auto shared_source = safe::make_shared<capture_context_t>(start, stop);

  class CaptureSourceLease: public testing::Test {
  protected:
    void SetUp() override {
      starts = 0;
      stops = 0;
    }

    void TearDown() override {
      EXPECT_EQ(starts.load(), stops.load());
    }

    std::unique_ptr<lease_t> acquire(source_e source) {
      return std::make_unique<lease_t>(source, shared_source, start, stop);
    }
  };
}  // namespace

TEST_F(CaptureSourceLease, MirrorJoinReusesProducerAndItsFallbackGeneration) {
  auto first = acquire(source_e::active_output);
  auto second = acquire(source_e::active_output);
  ASSERT_NE(first->get(), nullptr);
  ASSERT_EQ(first->get(), second->get());
  EXPECT_EQ(starts.load(), 1);

  // Both encoders must observe one reinit, e.g. the WGC-to-DXGI handoff,
  // rather than creating competing output duplications.
  first->get()->reinit.raise(true);
  EXPECT_TRUE(second->get()->reinit.peek());
  first.reset();
  EXPECT_EQ(stops.load(), 0);
  EXPECT_TRUE(second->get()->reinit.peek());
  second.reset();
  EXPECT_EQ(stops.load(), 1);
}

TEST_F(CaptureSourceLease, RemoteMonitorAndInputRemainPrivate) {
  auto game = acquire(source_e::active_output);
  auto monitor_a = acquire(source_e::exact_output);
  auto monitor_b = acquire(source_e::exact_output);
  auto input = acquire(source_e::synthetic_black);
  EXPECT_NE(game->get(), monitor_a->get());
  EXPECT_NE(monitor_a->get(), monitor_b->get());
  EXPECT_NE(input->get(), game->get());
  EXPECT_NE(input->get(), monitor_a->get());
  EXPECT_EQ(starts.load(), 4);
  monitor_a->get()->reinit.raise(true);
  EXPECT_FALSE(game->get()->reinit.peek());
  EXPECT_FALSE(monitor_b->get()->reinit.peek());
  EXPECT_FALSE(input->get()->reinit.peek());
  monitor_a.reset();
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(game->get()->generation, 1);
}

TEST_F(CaptureSourceLease, FinalDisconnectStopsAndReconnectStartsFresh) {
  {
    auto first = acquire(source_e::active_output);
    auto second = acquire(source_e::active_output);
    second.reset();
    EXPECT_EQ(stops.load(), 0);
    EXPECT_EQ(first->get()->generation, 1);
  }
  EXPECT_EQ(stops.load(), 1);
  auto reconnect = acquire(source_e::active_output);
  EXPECT_EQ(reconnect->get()->generation, 2);
  EXPECT_FALSE(reconnect->get()->reinit.peek());
}

TEST_F(CaptureSourceLease, ConcurrentJoinsStartOnlyOneProducer) {
  auto first = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  auto second = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  auto first_lease = first.get();
  auto second_lease = second.get();
  EXPECT_EQ(first_lease->get(), second_lease->get());
  EXPECT_EQ(starts.load(), 1);
  first_lease.reset();
  EXPECT_EQ(stops.load(), 0);
  second_lease.reset();
  EXPECT_EQ(stops.load(), 1);
}

TEST_F(CaptureSourceLease, FailedPrivateStartupDoesNotStopAnUnstartedWorker) {
  lease_t failed {
    source_e::exact_output,
    shared_source,
    [](capture_context_t &) {
      return -1;
    },
    stop
  };
  EXPECT_EQ(failed.get(), nullptr);
  EXPECT_EQ(starts.load(), 0);
  EXPECT_EQ(stops.load(), 0);
}

TEST_F(CaptureSourceLease, ReconnectWaitsForPreviousProducerTeardown) {
  using namespace std::chrono_literals;
  std::promise<void> entered_stop;
  std::promise<void> release_stop;
  auto entered = entered_stop.get_future();
  auto release = release_stop.get_future();
  stop_hook = [&] {
    entered_stop.set_value();
    release.wait();
  };
  auto first = acquire(source_e::active_output);
  auto disconnect = std::async(std::launch::async, [&] {
    first.reset();
  });
  EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
  auto reconnect = std::async(std::launch::async, [&] {
    return acquire(source_e::active_output);
  });
  EXPECT_EQ(reconnect.wait_for(10ms), std::future_status::timeout);
  EXPECT_EQ(starts.load(), 1);
  release_stop.set_value();
  disconnect.get();
  auto next = reconnect.get();
  stop_hook = {};
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(next->get()->generation, 2);
}

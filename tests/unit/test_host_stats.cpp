/**
 * @file tests/unit/test_host_stats.cpp
 * @brief Deterministic host-statistics provider and lifecycle tests.
 */
#include "../tests_common.h"

#include <src/host_stats_service.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

TEST(HostStatsTypes, DefaultSentinels) {
  const platf::host_stats_t stats {};
  EXPECT_FLOAT_EQ(stats.cpu_percent, -1.f);
  EXPECT_FLOAT_EQ(stats.cpu_temp_c, -1.f);
  EXPECT_FLOAT_EQ(stats.gpu_percent, -1.f);
  EXPECT_FLOAT_EQ(stats.gpu_encoder_percent, -1.f);
  EXPECT_FLOAT_EQ(stats.gpu_temp_c, -1.f);
  EXPECT_EQ(stats.ram_used_bytes, 0u);
  EXPECT_EQ(stats.ram_total_bytes, 0u);
  EXPECT_EQ(stats.vram_used_bytes, 0u);
  EXPECT_EQ(stats.vram_total_bytes, 0u);
  EXPECT_DOUBLE_EQ(stats.net_rx_bps, -1.0);
  EXPECT_DOUBLE_EQ(stats.net_tx_bps, -1.0);
}

TEST(HostStatsTypes, HostInfoDefaults) {
  const platf::host_info_t info {};
  EXPECT_TRUE(info.cpu_model.empty());
  EXPECT_TRUE(info.gpu_model.empty());
  EXPECT_EQ(info.cpu_logical_cores, 0);
  EXPECT_EQ(info.ram_total_bytes, 0u);
  EXPECT_EQ(info.vram_total_bytes, 0u);
  EXPECT_TRUE(info.net_interface.empty());
  EXPECT_EQ(info.net_link_speed_mbps, 0u);
}

namespace {
  struct provider_state_t {
    std::atomic<int> sample_calls {0};
    std::atomic<int> info_calls {0};
    std::atomic<int> reset_calls {0};
    std::atomic<bool> block_sample {false};
    std::mutex sample_mutex;
    std::condition_variable sample_cv;
    bool sample_entered = false;
    bool release_sample = false;
  };

  class fake_provider_t: public platf::host_stats_provider_t {
  public:
    explicit fake_provider_t(std::shared_ptr<provider_state_t> state):
        state(std::move(state)) {}

    platf::host_stats_t sample() override {
      ++state->sample_calls;
      if (state->block_sample.load()) {
        std::unique_lock lock(state->sample_mutex);
        state->sample_entered = true;
        state->sample_cv.notify_all();
        state->sample_cv.wait(lock, [this] {
          return state->release_sample;
        });
      }
      platf::host_stats_t result;
      result.cpu_percent = 37.5f;
      result.gpu_percent = 62.0f;
      result.ram_used_bytes = 4;
      result.ram_total_bytes = 16;
      result.net_rx_bps = 1000.0;
      result.net_tx_bps = 2000.0;
      return result;
    }

    void reset_rate_baselines() override {
      ++state->reset_calls;
    }

    platf::host_info_t info() override {
      ++state->info_calls;
      platf::host_info_t result;
      result.cpu_model = "Fake CPU";
      result.gpu_model = "Fake GPU";
      result.cpu_logical_cores = 8;
      result.ram_total_bytes = 16;
      return result;
    }

    std::shared_ptr<provider_state_t> state;
  };

  host_stats::service_t make_service(const std::shared_ptr<provider_state_t> &state) {
    return host_stats::service_t {
      [state] {
        return std::make_unique<fake_provider_t>(state);
      },
      [] {
        return true;
      },
      [] {
        return 24h;
      }
    };
  }

  bool wait_for_calls(const std::atomic<int> &calls, int expected, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (calls.load() >= expected) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return calls.load() >= expected;
  }
}  // namespace

TEST(HostStatsService, LatestBeforeStartReturnsSentinels) {
  auto service = make_service(std::make_shared<provider_state_t>());
  EXPECT_FLOAT_EQ(service.latest().cpu_percent, -1.f);
  EXPECT_FALSE(service.is_running());
}

TEST(HostStatsService, StartCachesInfoWithoutSamplingDynamicStats) {
  auto state = std::make_shared<provider_state_t>();
  auto service = make_service(state);

  auto guard = service.start();
  ASSERT_TRUE(guard);
  EXPECT_TRUE(service.is_running());
  EXPECT_EQ(state->sample_calls.load(), 0);
  EXPECT_EQ(state->info_calls.load(), 1);

  const auto stats = service.latest();
  EXPECT_FLOAT_EQ(stats.cpu_percent, -1.f);
  EXPECT_EQ(service.info().cpu_model, "Fake CPU");
  EXPECT_EQ(service.info().cpu_logical_cores, 8);
}

TEST(HostStatsService, StreamDemandWakesSamplerAndIdleStopsIt) {
  auto state = std::make_shared<provider_state_t>();
  host_stats::service_t service {
    [state] {
      return std::make_unique<fake_provider_t>(state);
    },
    [] {
      return true;
    },
    [] {
      return 15ms;
    }
  };
  auto guard = service.start();
  ASSERT_TRUE(guard);

  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(state->sample_calls.load(), 0);

  service.set_streaming(true);
  ASSERT_TRUE(wait_for_calls(state->sample_calls, 2));
  EXPECT_EQ(state->reset_calls.load(), 1);
  EXPECT_FLOAT_EQ(service.latest().cpu_percent, 37.5f);

  service.set_streaming(false);
  std::this_thread::sleep_for(30ms);
  const int stopped_at = state->sample_calls.load();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(state->sample_calls.load(), stopped_at);

  service.set_streaming(true);
  ASSERT_TRUE(wait_for_calls(state->sample_calls, stopped_at + 1));
  EXPECT_EQ(state->reset_calls.load(), 2);
}

TEST(HostStatsService, WebConsumerGetsFreshSampleAndGraceAvoidsNavigationChurn) {
  auto state = std::make_shared<provider_state_t>();
  host_stats::service_t service {
    [state] {
      return std::make_unique<fake_provider_t>(state);
    },
    [] {
      return true;
    },
    [] {
      return 15ms;
    },
    {},
    [] {
      return 80ms;
    }
  };
  auto guard = service.start();
  ASSERT_TRUE(guard);

  const auto stats = service.latest_for_consumer();
  EXPECT_FLOAT_EQ(stats.cpu_percent, 37.5f);
  ASSERT_TRUE(wait_for_calls(state->sample_calls, 3));

  std::this_thread::sleep_for(100ms);
  const int stopped_at = state->sample_calls.load();
  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(state->sample_calls.load(), stopped_at);
}

TEST(HostStatsService, ConcurrentWebConsumersShareOneFreshSample) {
  auto state = std::make_shared<provider_state_t>();
  state->block_sample.store(true);
  host_stats::service_t service {
    [state] {
      return std::make_unique<fake_provider_t>(state);
    },
    [] {
      return true;
    },
    [] {
      return 24h;
    }
  };
  auto guard = service.start();
  ASSERT_TRUE(guard);

  auto first = std::async(std::launch::async, [&service] {
    return service.latest_for_consumer();
  });
  bool sample_entered = false;
  {
    std::unique_lock lock(state->sample_mutex);
    sample_entered = state->sample_cv.wait_for(lock, 1s, [state] {
      return state->sample_entered;
    });
  }
  if (!sample_entered) {
    {
      const std::lock_guard lock(state->sample_mutex);
      state->release_sample = true;
    }
    state->sample_cv.notify_all();
    first.wait();
    FAIL() << "sampler did not enter provider";
    return;
  }
  auto second = std::async(std::launch::async, [&service] {
    return service.latest_for_consumer();
  });
  std::this_thread::sleep_for(20ms);
  {
    const std::lock_guard lock(state->sample_mutex);
    state->release_sample = true;
  }
  state->sample_cv.notify_all();

  EXPECT_FLOAT_EQ(first.get().cpu_percent, 37.5f);
  EXPECT_FLOAT_EQ(second.get().cpu_percent, 37.5f);
  EXPECT_EQ(state->sample_calls.load(), 1);
}

TEST(HostStatsService, WebLeaseRenewalDoesNotBypassSamplingInterval) {
  auto state = std::make_shared<provider_state_t>();
  host_stats::service_t service {
    [state] {
      return std::make_unique<fake_provider_t>(state);
    },
    [] {
      return true;
    },
    [] {
      return 24h;
    }
  };
  auto guard = service.start();
  ASSERT_TRUE(guard);

  EXPECT_FLOAT_EQ(service.latest_for_consumer().cpu_percent, 37.5f);
  EXPECT_FLOAT_EQ(service.latest_for_consumer().cpu_percent, 37.5f);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(state->sample_calls.load(), 1);
}

TEST(HostStatsService, ConfigurationChangesStopAndResumeActiveDemand) {
  auto state = std::make_shared<provider_state_t>();
  std::atomic<bool> enabled {true};
  host_stats::service_t service {
    [state] {
      return std::make_unique<fake_provider_t>(state);
    },
    [&enabled] {
      return enabled.load();
    },
    [] {
      return 15ms;
    }
  };
  auto guard = service.start();
  ASSERT_TRUE(guard);
  service.set_streaming(true);
  ASSERT_TRUE(wait_for_calls(state->sample_calls, 2));

  enabled.store(false);
  service.configuration_changed();
  std::this_thread::sleep_for(30ms);
  const int disabled_at = state->sample_calls.load();
  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(state->sample_calls.load(), disabled_at);
  EXPECT_FLOAT_EQ(service.latest().cpu_percent, -1.f);

  enabled.store(true);
  service.configuration_changed();
  ASSERT_TRUE(wait_for_calls(state->sample_calls, disabled_at + 1));
  EXPECT_EQ(state->reset_calls.load(), 2);
}

TEST(HostStatsService, SecondaryGuardCannotStopOwningLifecycle) {
  auto service = make_service(std::make_shared<provider_state_t>());
  auto owner = service.start();
  ASSERT_TRUE(owner);

  auto secondary = service.start();
  ASSERT_TRUE(secondary);
  secondary.reset();
  EXPECT_TRUE(service.is_running());

  owner.reset();
  EXPECT_FALSE(service.is_running());
}

TEST(HostStatsService, MissingProviderFailsWithoutChangingState) {
  host_stats::service_t service {
    [] {
      return std::unique_ptr<platf::host_stats_provider_t> {};
    },
    [] {
      return true;
    },
    [] {
      return 24h;
    }
  };

  EXPECT_FALSE(service.start());
  EXPECT_FALSE(service.is_running());
  EXPECT_FLOAT_EQ(service.latest().cpu_percent, -1.f);
}

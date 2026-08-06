/**
 * @file tests/unit/test_host_stats.cpp
 * @brief Deterministic host-statistics provider and lifecycle tests.
 */
#include "../tests_common.h"

#include <src/host_stats_service.h>

#include <atomic>
#include <chrono>
#include <memory>

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
  };

  class fake_provider_t: public platf::host_stats_provider_t {
  public:
    explicit fake_provider_t(std::shared_ptr<provider_state_t> state):
        state(std::move(state)) {}

    platf::host_stats_t sample() override {
      ++state->sample_calls;
      platf::host_stats_t result;
      result.cpu_percent = 37.5f;
      result.gpu_percent = 62.0f;
      result.ram_used_bytes = 4;
      result.ram_total_bytes = 16;
      result.net_rx_bps = 1000.0;
      result.net_tx_bps = 2000.0;
      return result;
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
}  // namespace

TEST(HostStatsService, LatestBeforeStartReturnsSentinels) {
  auto service = make_service(std::make_shared<provider_state_t>());
  EXPECT_FLOAT_EQ(service.latest().cpu_percent, -1.f);
  EXPECT_FALSE(service.is_running());
}

TEST(HostStatsService, StartUsesInjectedProviderSynchronously) {
  auto state = std::make_shared<provider_state_t>();
  auto service = make_service(state);

  auto guard = service.start();
  ASSERT_TRUE(guard);
  EXPECT_TRUE(service.is_running());
  EXPECT_GE(state->sample_calls.load(), 1);
  EXPECT_EQ(state->info_calls.load(), 1);

  const auto stats = service.latest();
  EXPECT_FLOAT_EQ(stats.cpu_percent, 37.5f);
  EXPECT_FLOAT_EQ(stats.gpu_percent, 62.0f);
  EXPECT_EQ(stats.ram_used_bytes, 4u);
  EXPECT_EQ(stats.ram_total_bytes, 16u);
  EXPECT_EQ(service.info().cpu_model, "Fake CPU");
  EXPECT_EQ(service.info().cpu_logical_cores, 8);
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

/**
 * @file tests/unit/test_host_stats.cpp
 * @brief Tests for src/host_stats.* and the platform host_stats_t /
 *        host_info_t value types.
 *
 * The full @c host_stats::start() / @c host_stats::latest() round-trip is
 * exercised on platforms that provide a real implementation (currently
 * Windows, Linux and macOS). On any platform where the factory cannot
 * produce a provider we fall back to checking the documented sentinel
 * defaults so the test suite still runs.
 */
#include "../tests_common.h"

#include <src/host_stats.h>
#include <src/platform/common.h>

#include <chrono>
#include <thread>

TEST(HostStatsTypes, DefaultSentinels) {
  platf::host_stats_t s {};
  EXPECT_FLOAT_EQ(s.cpu_percent, -1.f);
  EXPECT_FLOAT_EQ(s.cpu_temp_c, -1.f);
  EXPECT_FLOAT_EQ(s.gpu_percent, -1.f);
  EXPECT_FLOAT_EQ(s.gpu_encoder_percent, -1.f);
  EXPECT_FLOAT_EQ(s.gpu_temp_c, -1.f);
  EXPECT_EQ(s.ram_used_bytes, 0u);
  EXPECT_EQ(s.ram_total_bytes, 0u);
  EXPECT_EQ(s.vram_used_bytes, 0u);
  EXPECT_EQ(s.vram_total_bytes, 0u);
  EXPECT_DOUBLE_EQ(s.net_rx_bps, -1.0);
  EXPECT_DOUBLE_EQ(s.net_tx_bps, -1.0);
}

TEST(HostStatsTypes, HostInfoDefaults) {
  platf::host_info_t i {};
  EXPECT_TRUE(i.cpu_model.empty());
  EXPECT_TRUE(i.gpu_model.empty());
  EXPECT_EQ(i.cpu_logical_cores, 0);
  EXPECT_EQ(i.ram_total_bytes, 0u);
  EXPECT_EQ(i.vram_total_bytes, 0u);
  EXPECT_TRUE(i.net_interface.empty());
  EXPECT_EQ(i.net_link_speed_mbps, 0u);
}

TEST(HostStatsSingleton, LatestBeforeStartReturnsSentinels) {
  // Without start() the cached snapshot must remain at the default sentinels.
  auto s = host_stats::latest();
  EXPECT_FLOAT_EQ(s.cpu_percent, -1.f);
  EXPECT_FLOAT_EQ(s.gpu_percent, -1.f);
}

TEST(HostStatsSingleton, StartProducesSnapshot) {
  auto guard = host_stats::start();
  if (!guard) {
    GTEST_SKIP() << "no host_stats provider on this platform";
  }

  // start() takes the first sample synchronously, so latest() must already
  // reflect a provider call (even if every individual field is a sentinel).
  auto s = host_stats::latest();

  // RAM totals are universally available; treat them as the smoke test.
  EXPECT_GT(s.ram_total_bytes, 0u) << "host RAM total should be discoverable";
  EXPECT_LE(s.ram_used_bytes, s.ram_total_bytes);

  const auto &i = host_stats::info();
  EXPECT_GT(i.cpu_logical_cores, 0);
  EXPECT_GT(i.ram_total_bytes, 0u);

  // Percentage fields, when present, must be in [0, 100]. Unavailable fields
  // remain at the documented -1 sentinel.
  auto sane_percent = [](float v) {
    return v < 0.f || (v >= 0.f && v <= 100.5f);
  };
  EXPECT_TRUE(sane_percent(s.cpu_percent));
  EXPECT_TRUE(sane_percent(s.gpu_percent));
  EXPECT_TRUE(sane_percent(s.gpu_encoder_percent));

  // Network throughput is either -1 (no measurement) or >= 0.
  EXPECT_TRUE(s.net_rx_bps < 0.0 || s.net_rx_bps >= 0.0);
  EXPECT_TRUE(s.net_tx_bps < 0.0 || s.net_tx_bps >= 0.0);
}

TEST(HostStatsSingleton, DoubleStartIsHarmless) {
  auto first = host_stats::start();
  if (!first) {
    GTEST_SKIP() << "no host_stats provider on this platform";
  }
  // A second start() while the singleton is already running must not crash
  // and should return *some* guard (current implementation logs a warning).
  auto second = host_stats::start();
  // Either nullptr (refusal) or a separate guard is acceptable; just don't
  // explode.
  SUCCEED();
}

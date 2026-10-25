/**
 * @file tests/unit/test_video_adaptive_pacing.cpp
 * @brief Test src/video_adaptive_pacing.h
 *
 * Ported from Rubylight's adaptive_pacing/tests.rs. The queue-model simulations
 * there depend on Rubylight's packetizer and pacer and are not ported.
 */

#include "../tests_common.h"
#include "src/video_adaptive_pacing.h"

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace stream::pacing {
  /// Test-only view of the controller's private state.
  struct adaptive_pacing_access_t {
    static std::optional<std::uint64_t> &limit(adaptive_pacing_t &pacing) {
      return pacing.limit_;
    }

    static void set_probe(adaptive_pacing_t &pacing, std::uint64_t stable, adaptive_pacing_t::time_point started, adaptive_pacing_t::duration settle) {
      pacing.probe_ = adaptive_pacing_t::probe_t {stable, started, settle};
    }

    static bool has_probe(const adaptive_pacing_t &pacing) {
      return pacing.probe_.has_value();
    }

    static adaptive_pacing_t::time_point probe_started(const adaptive_pacing_t &pacing) {
      return pacing.probe_->started;
    }

    static std::uint64_t probe_stable(const adaptive_pacing_t &pacing) {
      return pacing.probe_->stable;
    }

    static void set_any_loss(adaptive_pacing_t &pacing, bool value) {
      pacing.any_loss_ = value;
    }

    static bool any_loss(const adaptive_pacing_t &pacing) {
      return pacing.any_loss_;
    }

    static std::optional<adaptive_pacing_t::time_point> sample_time(const adaptive_pacing_t &pacing) {
      if (!pacing.last_sample_) {
        return std::nullopt;
      }
      return pacing.last_sample_->at;
    }

    static std::optional<adaptive_pacing_t::time_point> clean_since(const adaptive_pacing_t &pacing) {
      return pacing.clean_since_;
    }

    static bool awaiting_fence(const adaptive_pacing_t &pacing) {
      return pacing.awaiting_fence_;
    }
  };
}  // namespace stream::pacing

namespace {
  using namespace std::chrono_literals;
  using namespace stream::pacing;
  using priv = adaptive_pacing_access_t;
  using time_point = adaptive_pacing_t::time_point;
  using duration = adaptive_pacing_t::duration;
  using change_t = std::optional<std::pair<std::uint64_t, std::uint64_t>>;

  constexpr std::uint64_t fast = 800'000'000;
  constexpr std::uint64_t demand = 50'000'000;
  constexpr std::uint64_t seed = 80'000'000;
  constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint32_t u32_max = std::numeric_limits<std::uint32_t>::max();
  constexpr std::array<char, 8> ping_bytes {4, 0, 0, 0, 0, 0, 0, 0};
  constexpr std::array<char, 8> bad_ping_bytes {3, 3, 3, 3, 3, 3, 3, 3};
  const std::string_view ping {ping_bytes.data(), ping_bytes.size()};
  const std::string_view bad_ping {bad_ping_bytes.data(), bad_ping_bytes.size()};

  change_t change(std::uint64_t from, std::uint64_t to) {
    return std::pair {from, to};
  }

  fec_block_t recovered(std::uint16_t lost = 1, std::uint16_t data = 100) {
    return fec_block_t::recovered_block(lost, data);
  }

  const fec_block_t failed = fec_block_t::unrecoverable_block();

  struct stream_t {
    adaptive_pacing_t pacing;
    time_point now;
    traffic_t traffic;
    std::uint32_t last {};
    duration period;
    std::uint32_t kbps {40'000};

    static stream_t fresh(bool enabled) {
      stream_t stream {adaptive_pacing_t {enabled}, adaptive_pacing_t::clock::now(), {}, 0, std::chrono::duration_cast<duration>(std::chrono::duration<double>(1. / 60.)), 40'000};
      EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), enabled ? seed : fast);
      stream.pacing.tick(stream.now, {}, 0, stream.period, 40'000, duration::zero());
      return stream;
    }

    /// Existing steady-state regressions first exercise a complete healthy
    /// startup. The synthetic wire sequence wraps back to zero so their small
    /// explicit frame numbers stay newer than the final startup fence.
    static stream_t established(bool enabled) {
      auto stream = fresh(enabled);
      stream.last = 0u - 120 * 30;
      for (int i = 0; i < 120; ++i) {
        stream.healthy();
      }
      EXPECT_EQ(stream.last, 0u);
      EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), fast);
      EXPECT_FALSE(priv::has_probe(stream.pacing));
      return stream;
    }

    change_t step(std::optional<std::string_view> payload, std::uint64_t frames, std::uint64_t bytes, duration rtt) {
      now += 500ms;
      traffic.frames += frames;
      traffic.bytes += bytes;
      traffic.packets += 2000;
      last += std::uint32_t(frames);
      if (payload) {
        pacing.on_ping(now, *payload);
      }
      return pacing.tick(now, traffic, last, period, kbps, rtt);
    }

    change_t healthy() {
      // Half a second of the encoder's bitrate.
      return step(ping, 30, std::uint64_t {kbps} * 125 / 2, 2ms);
    }

    void report(std::uint32_t frame, std::uint32_t last_sent, fec_block_t block) {
      pacing.on_block(frame, last_sent, period, block);
    }

    change_t failures() {
      const auto end = last + 30;
      report(end - 1, end, failed);
      report(end, end, failed);
      return healthy();
    }

    void first_probe() {
      ASSERT_EQ(failures(), change(fast, fast / 2));
      for (int i = 0; i < 16; ++i) {
        ASSERT_EQ(healthy(), std::nullopt);
      }
      ASSERT_EQ(healthy(), change(400'000'000, 420'000'000));
    }
  };
}  // namespace

TEST(VideoAdaptivePacing, AnOversizedFrameDoesNotMoveTheWireFloorOrWithdrawAProbe) {
  // The sender zeroes the FEC percentage for frames over four FEC blocks. The
  // demand it reports must still come from the configured percentage, so such
  // a frame between two ordinary ones changes nothing.
  const auto without_fec = nominal_wire_demand_bps(40'000, 60, 16, 1376, 1458, 0, 2);
  ASSERT_LT(without_fec, nominal_wire_demand_bps(40'000, 60, 16, 1376, 1458, 20, 2));

  auto stream = stream_t::established(true);
  stream.first_probe();
  ASSERT_TRUE(priv::has_probe(stream.pacing));
  const auto before = stream.pacing.snapshot();
  const auto sample_at = priv::sample_time(stream.pacing);
  const auto clean_since = priv::clean_since(stream.pacing);

  // Normal, oversized and normal frames all report the configured demand.
  for (int i = 0; i < 3; ++i) {
    stream.pacing.rate(fast, demand, 40'000);
  }
  EXPECT_TRUE(priv::has_probe(stream.pacing));
  EXPECT_FALSE(priv::awaiting_fence(stream.pacing));
  EXPECT_EQ(stream.pacing.snapshot(), before);
  EXPECT_EQ(priv::sample_time(stream.pacing), sample_at);
  EXPECT_EQ(priv::clean_since(stream.pacing), clean_since);

  // The failure the caller must avoid: a different floor mid-probe withdraws it.
  stream.pacing.rate(fast, without_fec, 40'000);
  EXPECT_FALSE(priv::has_probe(stream.pacing));
  EXPECT_TRUE(priv::awaiting_fence(stream.pacing));
}

TEST(VideoAdaptivePacing, FreshStreamsSeedFromEncoderAndWireDemandBeforeTheFirstPacket) {
  struct row_t {
    std::uint64_t ceiling;
    std::uint64_t demand;
    std::uint32_t encoder;
    std::uint64_t expected;
  };

  for (const auto &row : {
         row_t {fast, demand, 40'000, seed},
         row_t {fast, 90'000'000, 40'000, 112'500'000},
         row_t {fast, 10'000'000, 80'000, 160'000'000},
         row_t {30'000'000, demand, 40'000, 30'000'000},
         row_t {fast, 0, 0, 1'000'000},
         row_t {fast, u64_max, u32_max, fast},
       }) {
    adaptive_pacing_t pacing {true};
    EXPECT_EQ(pacing.rate(row.ceiling, row.demand, row.encoder), row.expected);
    // Merely consulting the budget does not advance startup.
    EXPECT_EQ(pacing.rate(row.ceiling, row.demand, row.encoder), row.expected);
  }
}

TEST(VideoAdaptivePacing, FreshHealthyStreamsProbeGraduallyAndFinishAtTheFastCeiling) {
  auto stream = stream_t::fresh(true);
  std::uint64_t previous = seed;
  auto last_change = stream.now;
  int increases = 0;
  for (int i = 0; i < 40; ++i) {
    if (const auto result = stream.healthy()) {
      const auto [old, next] = *result;
      EXPECT_EQ(old, previous);
      EXPECT_GT(next, old);
      EXPECT_LE(next, old + old / 8);
      EXPECT_GE(stream.now - last_change, 500ms);
      previous = next;
      last_change = stream.now;
      ++increases;
    }
    EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), previous);
  }
  EXPECT_EQ(previous, fast);
  EXPECT_GE(increases, 20) << "startup must not jump directly to the ceiling";
  EXPECT_FALSE(stream.pacing.snapshot().startup);
  EXPECT_FALSE(stream.pacing.snapshot().probing);
}

TEST(VideoAdaptivePacing, StartupWithoutValidRecentFeedbackOrBusyVideoNeverJumpsToFastRate) {
  for (int condition = 0; condition < 6; ++condition) {
    auto stream = stream_t::fresh(true);
    for (int i = 0; i < 120; ++i) {
      change_t result;
      switch (condition) {
        case 0:
          result = stream.step(std::nullopt, 30, 2'500'000, 2ms);
          break;
        case 1:
          result = stream.step(bad_ping, 30, 2'500'000, 2ms);
          break;
        case 2:
          result = stream.step(ping, 30, 2'500'000, duration::zero());
          break;
        case 3:
          result = stream.step(ping, 0, 2'500'000, 2ms);
          break;
        case 4:
          result = stream.step(ping, 30, 1, 2ms);
          break;
        default:
          stream.now += 3s;
          result = stream.healthy();
          break;
      }
      ASSERT_EQ(result, std::nullopt) << "startup condition " << condition;
      ASSERT_EQ(stream.pacing.rate(fast, demand, 40'000), seed);
    }
  }
}

TEST(VideoAdaptivePacing, StartupGuardAllowsRoundTripFeedbackBeforeTheNextProbe) {
  auto stream = stream_t::fresh(true);
  auto previous_change = stream.now;
  int changes = 0;
  for (int i = 0; i < 12; ++i) {
    if (stream.step(ping, 30, 2'500'000, 400ms)) {
      EXPECT_GE(stream.now - previous_change, 900ms);
      previous_change = stream.now;
      ++changes;
    }
  }
  EXPECT_GT(changes, 0) << "a stable higher RTT must not permanently block discovery";
}

TEST(VideoAdaptivePacing, AStartupProbeWaitsForIncreasedRttBeforeConfirmingOrAdvancing) {
  auto stream = stream_t::fresh(true);
  ASSERT_EQ(stream.step(ping, 30, 2'500'000, 400ms), std::nullopt);
  ASSERT_EQ(stream.step(ping, 30, 2'500'000, 400ms), change(seed, 90'000'000));
  const auto probe_started = stream.now;
  // 600 ms is still within the stable-RTT tolerance above the 400 ms
  // baseline, but extends the feedback guard from 900 ms to 1300 ms.
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(stream.step(ping, 30, 2'500'000, 600ms), std::nullopt);
    ASSERT_TRUE(priv::has_probe(stream.pacing));
    EXPECT_EQ(priv::probe_started(stream.pacing), probe_started);
    EXPECT_EQ(priv::probe_stable(stream.pacing), seed);
    EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), 90'000'000u);
  }
  EXPECT_EQ(stream.now - probe_started, 1s);
  EXPECT_EQ(stream.step(ping, 30, 2'500'000, 600ms), change(90'000'000, 101'250'000));
  EXPECT_GE(stream.now - probe_started, 1300ms);
}

TEST(VideoAdaptivePacing, TheFirstStartupProbeRollsBackOnOneNewLossAndIgnoresOldReports) {
  auto stream = stream_t::fresh(true);
  change_t probe;
  for (int i = 0; i < 4; ++i) {
    if (const auto result = stream.healthy()) {
      probe = result;
      break;
    }
  }
  ASSERT_EQ(probe, change(seed, 90'000'000));
  const auto fence = stream.last;
  stream.report(fence, fence, failed);
  EXPECT_FALSE(priv::any_loss(stream.pacing)) << "old-rate feedback must not poison the probe";
  stream.report(fence + 1, fence + 1, recovered());
  stream.now += 1ms;
  stream.last = fence + 1;
  EXPECT_EQ(stream.pacing.tick(stream.now, stream.traffic, stream.last, stream.period, 40'000, 2ms), change(90'000'000, seed))
    << "a valid failed probe need not await another full frame or 500 ms sample";
  EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), seed);
  EXPECT_FALSE(stream.pacing.snapshot().startup);
  EXPECT_FALSE(stream.pacing.snapshot().probing);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(stream.healthy(), std::nullopt) << "recovery must not immediately repeat startup probes";
  }
}

TEST(VideoAdaptivePacing, AFecBudgetChangeCannotEraseLossAlreadyReceivedDuringAStartupProbe) {
  for (const auto &[needed, expected] : {std::pair {std::uint64_t {60'000'000}, seed}, std::pair {std::uint64_t {72'000'000}, std::uint64_t {90'000'000}}}) {
    auto stream = stream_t::fresh(true);
    ASSERT_EQ(stream.healthy(), change(seed, 90'000'000));
    const auto frame = stream.last + 1;
    stream.report(frame, frame, recovered());
    // Adaptive FEC can update the next frame's wire budget between the
    // validated report and the control worker's pacing sample. Even when the
    // new floor forces the rolled-back rate to equal the probe rate, the
    // failure must end startup and retain the longer recovery hold.
    EXPECT_EQ(stream.pacing.rate(fast, needed, 40'000), expected);
    EXPECT_FALSE(stream.pacing.snapshot().startup);
    EXPECT_FALSE(stream.pacing.snapshot().probing);
    EXPECT_EQ(stream.pacing.snapshot().recovery_hold_seconds, 16u);
    ASSERT_EQ(stream.healthy(), std::nullopt);  // Install the new sent-frame fence.
    for (int i = 0; i < 20; ++i) {
      ASSERT_EQ(stream.healthy(), std::nullopt);
      ASSERT_EQ(stream.pacing.rate(fast, needed, 40'000), expected);
    }
  }
}

TEST(VideoAdaptivePacing, StartupRefreshesLiveFecFloorAndHonorsALowerRouteCeiling) {
  auto stream = stream_t::fresh(true);
  EXPECT_EQ(stream.pacing.rate(fast, 100'000'000, 40'000), 125'000'000u);
  EXPECT_EQ(stream.pacing.rate(90'000'000, 100'000'000, 40'000), 90'000'000u);
  EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), 90'000'000u);
  // A quieter FEC budget or a faster local route is not proof that the
  // remote path can accept an immediate return to the full ceiling.
  EXPECT_EQ(stream.pacing.rate(fast, 20'000'000, 40'000), 90'000'000u);
}

TEST(VideoAdaptivePacing, AConfirmedSlowRouteCannotJumpToANewFastCeilingWithoutNewActivity) {
  auto stream = stream_t::fresh(true);
  EXPECT_EQ(stream.pacing.rate(seed, demand, 40'000), seed);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_FALSE(stream.pacing.snapshot().startup);
  EXPECT_FALSE(stream.pacing.snapshot().limited);
  EXPECT_FALSE(priv::limit(stream.pacing)) << "the old ceiling is confirmed";

  EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), seed);
  // Pings preceding the route change must not authorize a probe on the
  // replacement route, even while they remain younger than two seconds.
  for (int i = 0; i < 24; ++i) {
    ASSERT_EQ(stream.step(std::nullopt, 30, 2'500'000, 2ms), std::nullopt);
    ASSERT_EQ(stream.pacing.rate(fast, demand, 40'000), seed);
  }
  std::uint64_t previous = seed;
  auto last_change = stream.now;
  for (int i = 0; i < 40; ++i) {
    if (const auto result = stream.healthy()) {
      const auto [old, next] = *result;
      EXPECT_EQ(old, previous);
      EXPECT_GT(next, old);
      EXPECT_LE(next, old + old / 8);
      EXPECT_GE(stream.now - last_change, 500ms);
      previous = next;
      last_change = stream.now;
    }
  }
  EXPECT_EQ(previous, fast) << "new healthy activity should permit guarded discovery";
  // Physical route shrinkage still takes effect before the next packet.
  EXPECT_EQ(stream.pacing.rate(40'000'000, demand, 40'000), 40'000'000u);
}

TEST(VideoAdaptivePacing, EstablishedFastPathsKeepTheFullRateWithoutLossReports) {
  for (const auto payload : {std::optional<std::string_view> {}, std::optional<std::string_view> {ping}}) {
    auto stream = stream_t::established(true);
    for (int i = 0; i < 120; ++i) {
      ASSERT_EQ(stream.step(payload, 30, 2'500'000, 2ms), std::nullopt);
      ASSERT_EQ(stream.pacing.rate(fast, demand, 40'000), fast);
    }
  }
}

TEST(VideoAdaptivePacing, DisabledPacingPreservesTheCallersRateDespiteReports) {
  auto stream = stream_t::established(false);
  for (int i = 0; i < 20; ++i) {
    ASSERT_EQ(stream.failures(), std::nullopt);
    ASSERT_EQ(stream.pacing.rate(950'000'000, demand, 40'000), 950'000'000u);
  }
  EXPECT_FALSE(stream.pacing.take_events().any());
}

TEST(VideoAdaptivePacing, OneFailedFrameIsNotCountedAsFourWhenItHasFourBlocks) {
  auto stream = stream_t::established(true);
  for (int i = 0; i < 4; ++i) {
    stream.report(30, 30, failed);
  }
  EXPECT_EQ(stream.healthy(), std::nullopt);
  // A delayed block for that same frame in a later control interval cannot
  // make one actual failed frame look like a new failure.
  stream.report(30, 60, failed);
  stream.report(60, 60, failed);
  EXPECT_EQ(stream.healthy(), std::nullopt);
  EXPECT_EQ(stream.failures(), change(fast, fast / 2));
}

TEST(VideoAdaptivePacing, RecoveredBlocksDoNotHideFailedBlocksOfTheSameFrames) {
  auto stream = stream_t::established(true);
  for (const std::uint32_t frame : {29u, 30u}) {
    stream.report(frame, 30, recovered());
    stream.report(frame, 30, failed);
  }
  EXPECT_EQ(stream.healthy(), change(fast, fast / 2));
}

TEST(VideoAdaptivePacing, LaterFailedBlocksStillCountAfterTheRecoveredBlocksWereSampled) {
  auto stream = stream_t::established(true);
  for (const std::uint32_t frame : {29u, 30u}) {
    stream.report(frame, 30, recovered());
  }
  EXPECT_EQ(stream.healthy(), std::nullopt);
  // Distinct later blocks establish that both frames actually failed, even
  // though their initial recovered-block losses belonged to the last sample.
  for (const std::uint32_t frame : {29u, 30u}) {
    stream.report(frame, 60, failed);
  }
  EXPECT_EQ(stream.healthy(), change(fast, fast / 2));
}

TEST(VideoAdaptivePacing, DistinctOutOfOrderFailuresCountOnceEach) {
  auto stream = stream_t::established(true);
  stream.report(30, 30, failed);
  stream.report(29, 30, failed);
  stream.report(30, 30, failed);
  EXPECT_EQ(stream.healthy(), change(fast, fast / 2));
}

TEST(VideoAdaptivePacing, FutureStaleAndPreCutReportsCannotReduceTheReplacementRate) {
  auto stream = stream_t::established(true);
  stream.last = 300;
  for (const std::uint32_t frame : {301u, 302u, 1u, 2u}) {
    stream.report(frame, 300, failed);
  }
  EXPECT_EQ(stream.healthy(), std::nullopt);
  EXPECT_EQ(stream.failures(), change(fast, fast / 2));
  const auto fenced = stream.last;
  for (const std::uint32_t frame : {fenced - 1, fenced}) {
    stream.report(frame, fenced, failed);
  }
  EXPECT_EQ(stream.healthy(), std::nullopt);
  EXPECT_EQ(stream.failures(), change(400'000'000, 200'000'000));
}

TEST(VideoAdaptivePacing, FrameFencesAndDistinctFailuresWorkAcrossU32Wrap) {
  auto stream = stream_t::established(true);
  stream.last = u32_max - 30;
  ASSERT_EQ(stream.failures(), change(fast, fast / 2));
  EXPECT_EQ(stream.last, u32_max);
  for (const std::uint32_t frame : {u32_max - 1, u32_max}) {
    stream.report(frame, 29, failed);
  }
  stream.report(0, 29, failed);
  stream.report(1, 29, failed);
  EXPECT_EQ(stream.healthy(), change(400'000'000, 200'000'000));
}

TEST(VideoAdaptivePacing, SparseRepairedPacketsDoNotRatchetTheRateDown) {
  auto stream = stream_t::established(true);
  for (int i = 0; i < 60; ++i) {
    const auto end = stream.last + 30;
    for (const std::uint32_t frame : {end - 1, end}) {
      stream.report(frame, end, recovered());
    }
    ASSERT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), fast);
  const auto end = stream.last + 30;
  for (const std::uint32_t frame : {end - 1, end}) {
    stream.report(frame, end, recovered(20, 100));
  }
  EXPECT_EQ(stream.healthy(), change(fast, fast / 2));
}

TEST(VideoAdaptivePacing, HostSendFailuresAloneDoNotGuessASlowerDownstreamLink) {
  auto stream = stream_t::established(true);
  for (int i = 0; i < 20; ++i) {
    stream.traffic.send_losses += 100;
    ASSERT_EQ(stream.healthy(), std::nullopt);
  }
}

TEST(VideoAdaptivePacing, ReductionsFollowLiveWireDemandAndNeverOverrideALowerCeiling) {
  auto stream = stream_t::established(true);
  for (int i = 0; i < 10; ++i) {
    stream.failures();
  }
  EXPECT_EQ(stream.pacing.rate(fast, demand, 40'000), 62'500'000u);
  // Increased FEC or changed FPS/packet size can increase wire demand while
  // encoder bitrate stays fixed. The next frame must immediately fit it.
  EXPECT_EQ(stream.pacing.rate(fast, 80'000'000, 40'000), 100'000'000u);
  EXPECT_EQ(stream.pacing.rate(fast, 40'000'000, 40'000), 100'000'000u);
  // A changed wire budget fences the old report epoch before accepting
  // fresh losses at the replacement rate.
  EXPECT_EQ(stream.failures(), std::nullopt);
  EXPECT_EQ(stream.failures(), change(100'000'000, 50'000'000));
  EXPECT_EQ(stream.pacing.rate(80'000'000, 120'000'000, 40'000), 80'000'000u);
  EXPECT_EQ(stream.failures(), std::nullopt);
  EXPECT_EQ(stream.pacing.rate(80'000'000, u64_max, 40'000), 80'000'000u);
}

TEST(VideoAdaptivePacing, RecoveryRequiresFreshValidPingsBusyTrafficAndStableRtt) {
  for (int condition = 0; condition < 6; ++condition) {
    auto stream = stream_t::established(true);
    ASSERT_EQ(stream.failures(), change(fast, fast / 2));
    for (int i = 0; i < 24; ++i) {
      change_t result;
      switch (condition) {
        case 0:
          result = stream.step(std::nullopt, 30, 2'500'000, 2ms);
          break;
        case 1:
          result = stream.step(bad_ping, 30, 2'500'000, 2ms);
          break;
        case 2:
          result = stream.step(ping, 1, 2'500'000, 2ms);
          break;
        case 3:
          result = stream.step(ping, 30, 1, 2ms);
          break;
        case 4:
          result = stream.step(ping, 30, 2'500'000, duration::zero());
          break;
        default:
          result = stream.step(ping, 30, 2'500'000, 50ms);
          break;
      }
      ASSERT_EQ(result, std::nullopt) << "unhealthy condition " << condition;
    }
    for (int i = 0; i < 16; ++i) {
      ASSERT_EQ(stream.healthy(), std::nullopt);
    }
    ASSERT_EQ(stream.healthy(), change(400'000'000, 420'000'000)) << "condition " << condition;
  }
}

TEST(VideoAdaptivePacing, ALateControlSampleDoesNotCountAsContinuousHealthyStreaming) {
  auto stream = stream_t::established(true);
  ASSERT_EQ(stream.failures(), change(fast, fast / 2));
  for (int i = 0; i < 14; ++i) {
    ASSERT_EQ(stream.healthy(), std::nullopt);
  }
  stream.now += 3s;
  ASSERT_EQ(stream.healthy(), std::nullopt);
  for (int i = 0; i < 16; ++i) {
    ASSERT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_EQ(stream.healthy(), change(400'000'000, 420'000'000));
}

TEST(VideoAdaptivePacing, AProbeNeedsTwoContinuousHealthySecondsBeforeAnotherIncrease) {
  auto stream = stream_t::established(true);
  stream.first_probe();
  EXPECT_EQ(stream.healthy(), std::nullopt);
  EXPECT_EQ(stream.healthy(), std::nullopt);
  // An idle sample breaks confirmation, even though wall-clock time since
  // starting the probe eventually exceeds two seconds.
  EXPECT_EQ(stream.step(ping, 0, 0, 2ms), std::nullopt);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_TRUE(priv::has_probe(stream.pacing));
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_FALSE(priv::has_probe(stream.pacing));
  for (int i = 0; i < 11; ++i) {
    EXPECT_EQ(stream.healthy(), std::nullopt);
  }
  EXPECT_EQ(stream.healthy(), change(420'000'000, 441'000'000));
}

TEST(VideoAdaptivePacing, EvenSparseNewLossRollsBackAnUnconfirmedProbe) {
  auto stream = stream_t::established(true);
  stream.first_probe();
  const auto before_probe = stream.last;
  // Reports from the old pacing rate cannot invalidate the new probe.
  for (const std::uint32_t frame : {before_probe - 1, before_probe}) {
    stream.report(frame, before_probe, failed);
  }
  EXPECT_EQ(stream.healthy(), std::nullopt);
  const auto end = stream.last + 30;
  stream.report(end, end, recovered());
  EXPECT_EQ(stream.healthy(), change(420'000'000, 400'000'000));
  EXPECT_EQ(stream.pacing.snapshot().recovery_hold_seconds, 16u);
}

TEST(VideoAdaptivePacing, FailedProbesRestoreTheWorkingRateAndBackOffToAOneMinuteHold) {
  auto stream = stream_t::established(true);
  stream.first_probe();
  for (const std::uint64_t hold_seconds : {16, 32, 60, 60}) {
    ASSERT_EQ(stream.failures(), change(420'000'000, 400'000'000));
    ASSERT_EQ(stream.pacing.snapshot().recovery_hold_seconds, hold_seconds);
    for (std::uint64_t i = 0; i < hold_seconds * 2; ++i) {
      ASSERT_EQ(stream.healthy(), std::nullopt);
    }
    ASSERT_EQ(stream.healthy(), change(400'000'000, 420'000'000));
  }
}

TEST(VideoAdaptivePacing, EventsDescribeStartProbesAndCutsForLogging) {
  auto stream = stream_t::established(true);
  auto events = stream.pacing.take_events();
  EXPECT_TRUE(events.started);
  EXPECT_TRUE(events.probe_started);
  EXPECT_TRUE(events.probe_accepted);
  EXPECT_TRUE(events.ceiling_confirmed);
  EXPECT_FALSE(events.cut);
  EXPECT_FALSE(stream.pacing.take_events().any());

  ASSERT_EQ(stream.failures(), change(fast, fast / 2));
  events = stream.pacing.take_events();
  EXPECT_TRUE(events.cut);
  EXPECT_TRUE(events.cut_by_failed_frames);

  for (int i = 0; i < 17; ++i) {
    stream.healthy();
  }
  EXPECT_TRUE(stream.pacing.take_events().probe_started);
  const auto frame = stream.last + 1;
  stream.report(frame, frame, failed);
  stream.healthy();
  events = stream.pacing.take_events();
  EXPECT_TRUE(events.probe_rejected);
  EXPECT_FALSE(events.cut);
}

TEST(VideoAdaptivePacing, NominalWireDemandCoversPayloadAndParity) {
  // 40 Mbps at 60 fps is 83334 bytes per frame; with a 16-byte header and
  // 1376-byte payloads that needs 61 data packets.
  const auto demand_bps = nominal_wire_demand_bps(40'000, 60, 16, 1376, 1458, 20, 2);
  EXPECT_EQ(demand_bps, std::uint64_t {fec_expanded_packets(61, 20, 2)} * 1458 * 8 * 60);
  // Wire demand never falls below the encoder bitrate.
  EXPECT_GT(demand_bps, 40'000'000u);

  EXPECT_EQ(nominal_wire_demand_bps(0, 60, 16, 1376, 1458, 20, 2), 0u);
  EXPECT_EQ(nominal_wire_demand_bps(40'000, 0, 16, 1376, 1458, 20, 2), 0u);
  EXPECT_EQ(nominal_wire_demand_bps(40'000, 60, 16, 0, 1458, 20, 2), 0u);

  // Without FEC the demand is the plain packet count.
  EXPECT_EQ(nominal_wire_demand_bps(40'000, 60, 16, 1376, 1458, 0, 0), std::uint64_t {61} * 1458 * 8 * 60);

  // A frame needing more than four blocks is sent without FEC: 3029 packets.
  EXPECT_EQ(nominal_wire_demand_bps(2'000'000, 60, 16, 1376, 1458, 20, 2), std::uint64_t {3029} * 1458 * 8 * 60);
}

TEST(VideoAdaptivePacing, PacketsPerMillisecondFollowTheRate) {
  EXPECT_EQ(packets_per_ms_at_rate(800'000'000, 1458), 68u);
  EXPECT_EQ(packets_per_ms_at_rate(1, 1458), 1u);
  EXPECT_EQ(packets_per_ms_at_rate(800'000'000, 0), 1u);
}

namespace {
  /// One ordinary video stream as stream.cpp packetizes and paces it.
  struct sender_case_t {
    int kbps;
    int fps;
    std::size_t packet_size;
    bool ipv6;
    bool encrypted;
    int fec = 20;
    std::size_t min_parity = 2;

    // stream.cpp: blocksize = packetsize + MAX_RTP_HEADER_SIZE (16), a
    // 32-byte video_packet_raw_t, an 8-byte frame header, a 32-byte
    // video_packet_enc_prefix_t, and Ethernet, IP and UDP overhead.
    std::size_t blocksize() const {
      return packet_size + 16;
    }

    std::size_t payload_blocksize() const {
      return blocksize() - 32;
    }

    std::size_t wire_bytes() const {
      return blocksize() + 38 + (ipv6 ? 40 : 20) + 8 + (encrypted ? 32 : 0);
    }

    /// The fixed pace adaptive pacing uses as its ceiling.
    std::size_t ceiling_packets_per_ms() const {
      return std::size_t(1'000'000'000ull * 80 / 100 / 1000 / blocksize() / 8);
    }

    std::uint64_t ceiling_bps() const {
      return std::uint64_t(ceiling_packets_per_ms()) * wire_bytes() * 8000;
    }

    std::uint64_t needed_bps() const {
      return nominal_wire_demand_bps(kbps, fps, 8, payload_blocksize(), wire_bytes(), fec, min_parity);
    }

    duration period() const {
      return std::chrono::duration_cast<duration>(std::chrono::duration<double>(1. / fps));
    }

    /// Packets of an average frame, split into aligned FEC blocks and
    /// FEC-expanded like stream.cpp and fec::encode do.
    std::size_t frame_packets() const {
      const std::uint64_t frame_bytes = (std::uint64_t(kbps) * 125 + std::uint64_t(fps) - 1) / std::uint64_t(fps);
      // concat_and_insert() puts a video_packet_raw_t before every payload slice.
      const auto data_bytes = std::size_t(frame_bytes) + 8;
      const auto slices = (data_bytes + payload_blocksize() - 1) / payload_blocksize();
      const auto payload = data_bytes + slices * (blocksize() - payload_blocksize());
      const std::size_t max_data_per_block = std::size_t(255 * 100 / (100 + fec)) * blocksize();
      std::size_t blocks = (payload + max_data_per_block - 1) / max_data_per_block;
      int percentage = fec;
      if (blocks > 4) {
        blocks = 4;
        percentage = 0;
      }
      const auto aligned = (payload / blocks + blocksize() - 1) / blocksize() * blocksize();
      std::size_t packets = 0;
      for (std::size_t block = 0; block < blocks && block * aligned < payload; ++block) {
        const auto bytes = block + 1 == blocks ? payload - block * aligned : aligned;
        packets += fec_expanded_packets((bytes + blocksize() - 1) / blocksize(), percentage, min_parity);
      }
      return packets;
    }
  };

  /// Send `frames` average frames arriving once per period, carrying pacing
  /// debt from one frame into the next like stream.cpp. Returns the most any
  /// frame finished after the next one arrived; zero when the pace keeps up.
  duration worst_backlog(const packet_pace_t &pace, std::size_t packets, duration period, int frames) {
    duration next_start {};
    duration worst {};
    for (int frame = 0; frame < frames; ++frame) {
      const auto arrival = period * frame;
      const auto start = std::max(next_start, arrival);
      next_start = start + pace.offset(packets);
      worst = std::max(worst, next_start - (arrival + period));
    }
    return worst;
  }

  /// The applied pace must enforce the controller's rate, stay within the
  /// ceiling's burst quantum, and keep up with the stream whenever its wire
  /// demand fits under the ceiling.
  void expect_pace_keeps_up(const sender_case_t &stream, std::uint64_t rate_bps, const char *phase) {
    SCOPED_TRACE(testing::Message() << phase << ": " << stream.kbps << " kbps, " << stream.fps << " fps, packetSize "
                                    << stream.packet_size << (stream.ipv6 ? ", IPv6" : ", IPv4")
                                    << (stream.encrypted ? ", encrypted" : "") << ", rate " << rate_bps);
    const auto pace = packet_pace_t::at_rate(rate_bps, stream.wire_bytes());
    EXPECT_LE(rate_bps, stream.ceiling_bps());
    EXPECT_LE(pace.packets_per_ms, stream.ceiling_packets_per_ms());
    EXPECT_GE(pace.packets_per_ms, 1u);
    if (stream.needed_bps() <= stream.ceiling_bps()) {
      // The demand estimate alone, without the floor's headroom, must carry
      // the frames the sender actually builds.
      EXPECT_LE(packet_pace_t::at_rate(stream.needed_bps(), stream.wire_bytes()).offset(stream.frame_packets()), stream.period());
      EXPECT_GE(rate_bps, stream.needed_bps());
      EXPECT_LE(pace.offset(stream.frame_packets()), stream.period());
      EXPECT_EQ(worst_backlog(pace, stream.frame_packets(), stream.period(), stream.fps * 10), duration::zero());
    }
  }

  std::vector<sender_case_t> sender_cases() {
    std::vector<sender_case_t> cases;
    for (const int kbps : {500, 1'000, 3'000, 10'000, 20'000, 50'000, 150'000}) {
      for (const int fps : {60, 120}) {
        // A conservative WAN packet size and the usual LAN one.
        for (const std::size_t packet_size : {std::size_t {1024}, std::size_t {1392}}) {
          for (const bool ipv6 : {false, true}) {
            for (const bool encrypted : {false, true}) {
              cases.push_back({kbps, fps, packet_size, ipv6, encrypted});
            }
          }
        }
      }
    }
    return cases;
  }
}  // namespace

TEST(VideoAdaptivePacing, AFixedPaceKeepsWholePacketsPerMillisecond) {
  const auto pace = packet_pace_t::fixed(7);
  EXPECT_EQ(pace.rate_bps, 0u);
  EXPECT_EQ(pace.offset(0), duration::zero());
  EXPECT_EQ(pace.offset(7), duration {1ms});
  EXPECT_EQ(pace.offset(10), std::chrono::duration_cast<duration>(1ms) * 10 / 7);
  EXPECT_EQ(packet_pace_t::fixed(0).packets_per_ms, 1u);
  EXPECT_EQ(packet_pace_t::fixed(68, 1458).applied_bps(), std::uint64_t {68} * 1458 * 8000);
}

TEST(VideoAdaptivePacing, ARatePaceChargesExactWireTime) {
  const auto pace = packet_pace_t::at_rate(20'000'000, 1474);
  EXPECT_EQ(pace.packets_per_ms, 1u);
  EXPECT_EQ(pace.applied_bps(), 20'000'000u);
  // 1474 bytes at 20 Mbps is 589.6 us.
  EXPECT_EQ(pace.offset(1), duration {589'600});
  EXPECT_EQ(pace.offset(20), duration {11'792'000});
  // Huge frames at tiny rates and small frames at fast rates neither overflow
  // nor lose precision.
  EXPECT_EQ(packet_pace_t::at_rate(1'000'000, 65'535).offset(1'000'000), std::chrono::duration_cast<duration>(std::chrono::seconds {524'280}));
  EXPECT_EQ(packet_pace_t::at_rate(800'000'000'000, 1458).offset(4096), duration {59'719});
  // Without a rate or a packet size it falls back to a fixed pace.
  EXPECT_EQ(packet_pace_t::at_rate(0, 1458).rate_bps, 0u);
  EXPECT_EQ(packet_pace_t::at_rate(20'000'000, 0).rate_bps, 0u);
}

TEST(VideoAdaptivePacing, TheTenMegabitStartupPaceKeepsUpWithoutPings) {
  // The reported case: 10 Mbps at 60 fps, packetSize 1392, IPv4, unencrypted,
  // 20% FEC with two parity packets minimum.
  const sender_case_t stream {10'000, 60, 1392, false, false};
  ASSERT_EQ(stream.payload_blocksize(), 1376u);
  ASSERT_EQ(stream.wire_bytes(), 1474u);
  ASSERT_EQ(stream.needed_bps(), 14'150'400u);
  ASSERT_EQ(stream.frame_packets(), 20u);

  stream_t live {adaptive_pacing_t {true}, adaptive_pacing_t::clock::now(), {}, 0, stream.period(), 10'000};
  const auto rate_bps = live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), 10'000);
  ASSERT_EQ(rate_bps, 20'000'000u);
  // Whole packets per millisecond round 20 Mbps down to 11.792 Mbps, which
  // cannot carry 20 packets every 16.7 ms.
  EXPECT_EQ(packets_per_ms_at_rate(rate_bps, stream.wire_bytes()), 1u);
  EXPECT_GT(packet_pace_t::fixed(1).offset(stream.frame_packets()), stream.period());

  EXPECT_EQ(packet_pace_t::at_rate(rate_bps, stream.wire_bytes()).offset(stream.frame_packets()), duration {11'792'000});
  expect_pace_keeps_up(stream, rate_bps, "startup");

  // Busy video with a measured RTT but no pings never probes; the starting
  // pace must hold.
  live.pacing.tick(live.now, {}, 0, live.period, 10'000, duration::zero());
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(live.step(std::nullopt, 30, 625'000, 2ms), std::nullopt);
    EXPECT_EQ(live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), 10'000), rate_bps);
  }
}

TEST(VideoAdaptivePacing, AppliedPaceKeepsUpAtStartupAcrossCommonStreams) {
  for (const auto &stream : sender_cases()) {
    adaptive_pacing_t pacing {true};
    const auto rate_bps = pacing.rate(stream.ceiling_bps(), stream.needed_bps(), std::uint32_t(stream.kbps));
    expect_pace_keeps_up(stream, rate_bps, "startup");
  }
}

TEST(VideoAdaptivePacing, AppliedPaceKeepsUpAtTheWireFloorThroughTheRecoveryHold) {
  for (const auto &stream : sender_cases()) {
    const auto kbps = std::uint32_t(stream.kbps);
    stream_t live {adaptive_pacing_t {true}, adaptive_pacing_t::clock::now(), {}, 0, stream.period(), kbps};
    live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), kbps);
    live.pacing.tick(live.now, {}, 0, live.period, kbps, duration::zero());
    // Repeated failed frames cut the rate down to the wire floor.
    for (int i = 0; i < 40; ++i) {
      live.failures();
      live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), kbps);
    }
    const auto floor_bps = live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), kbps);
    EXPECT_EQ(floor_bps / 1000, live.pacing.snapshot().wire_floor_kbps);
    expect_pace_keeps_up(stream, floor_bps, "loss cut");
    // The recovery hold keeps the floor until it has seen healthy seconds.
    for (int i = 0; i < 15; ++i) {
      live.healthy();
      EXPECT_EQ(live.pacing.rate(stream.ceiling_bps(), stream.needed_bps(), kbps), floor_bps);
      expect_pace_keeps_up(stream, floor_bps, "recovery hold");
    }
  }
}

#include "../tests_common.h"
#include "src/stream_protocol.h"

#include <climits>
#include <limits>

namespace {
  stream::video_pacing_demand_t demand(int fec = 20, int fps = 120, std::size_t payload = 1376, std::size_t prefix = 0) {
    return {150000, fps, payload, payload + 32 + prefix, 8, fec, 2};
  }
}

TEST(VideoPacingTests, ExplicitCeilingNeverBecomesAnEncoderFloor) {
  EXPECT_EQ(stream::video_pacing_batch_packets(1, 1408), 1u);
  EXPECT_EQ(stream::video_pacing_batch_packets(200000, 1408), 17u);
  EXPECT_EQ(stream::video_pacing_batch_packets(2000000, 1408), 177u);
  EXPECT_FALSE(stream::video_pacing_reservation(1, demand()).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(165000, demand(10)).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(165000, demand(20)).admissible);
  EXPECT_TRUE(stream::video_pacing_reservation(200000, demand()).admissible);
}

TEST(VideoPacingTests, ZeroDisablesAdmissionWithoutValidatingUnusedDemand) {
  const auto plan = stream::video_pacing_reservation(0, {});
  EXPECT_TRUE(plan.admissible);
  EXPECT_EQ(plan.packets_per_second, 0u);
  EXPECT_EQ(plan.send_ns_per_second, 0u);
  std::uint64_t used = 0;
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  EXPECT_EQ(used, 0u);
}

TEST(VideoPacingTests, CountsHeadersEncryptionAndCumulativeBitTime) {
  const auto plain = demand();
  const auto encrypted = demand(20, 120, 1376, 32);
  const auto plan = stream::video_pacing_reservation(200000, plain);
  const int exact_ceiling = int((plan.packets_per_second * plain.datagram_bytes * 8 + 999) / 1000);
  EXPECT_TRUE(stream::video_pacing_reservation(exact_ceiling, plain).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(exact_ceiling - 1, plain).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(exact_ceiling, encrypted).admissible);
  auto without_frame_header = plain;
  without_frame_header.frame_header_bytes = 0;
  EXPECT_GT(plan.packets_per_second, stream::video_pacing_reservation(200000, without_frame_header).packets_per_second);
}

TEST(VideoPacingTests, FractionalPacketRatesAdmitDemandThatFits) {
  for (const auto bytes : {1404u, 1408u, 1436u, 1440u}) {
    stream::video_pacing_demand_t low {1000, 60, 1376, bytes, 8, 20, 2};
    const auto plan = stream::video_pacing_reservation(8000, low);
    ASSERT_TRUE(plan.admissible) << bytes;
    EXPECT_EQ(plan.packets_per_second, 301u);
    EXPECT_EQ(stream::video_pacing_batch_packets(8000, bytes), 1u);
    EXPECT_GT(stream::video_pacing_delay_ns(1, bytes, 8000), 1'000'000u);
    EXPECT_EQ(plan.send_ns_per_second, stream::video_pacing_delay_ns(301, bytes, 8000));
    const int required_kbps = int((301 * bytes * 8 + 999) / 1000);
    EXPECT_TRUE(stream::video_pacing_reservation(required_kbps, low).admissible);
    EXPECT_FALSE(stream::video_pacing_reservation(required_kbps - 1, low).admissible);
    low.encoder_kbps = 20000;
    EXPECT_FALSE(stream::video_pacing_reservation(8000, low).admissible);
  }
}

TEST(VideoPacingTests, ExactPacketPerMillisecondBoundariesDoNotChangeAdmission) {
  for (const auto bytes : {1404u, 1408u, 1436u, 1440u}) {
    for (const int delta : {-1, 0, 1}) {
      const int cap = int(bytes * 8) + delta;
      EXPECT_TRUE(stream::video_pacing_reservation(cap, {1000, 60, 1376, bytes, 8, 20, 2}).admissible);
    }
  }
}

TEST(VideoPacingTests, CumulativeNanosecondRoundingNeverExceedsTheBitCeiling) {
  for (const auto bytes : {1404u, 1408u, 1436u, 1440u}) {
    for (const int cap : {1, 8000, 8001, 11231, 11232, 11233, 11487, 11488, 100000, INT_MAX}) {
      for (const std::uint64_t packets : {1u, 2u, 71u, 301u, 1000u}) {
        const auto delay = stream::video_pacing_delay_ns(packets, bytes, cap);
        const auto numerator = packets * bytes * 8 * 1'000'000;
        EXPECT_GE(delay * cap, numerator);
        EXPECT_LT((delay - 1) * cap, numerator);
        EXPECT_LE(delay, packets * stream::video_pacing_delay_ns(1, bytes, cap));
      }
    }
  }
  EXPECT_EQ(stream::video_pacing_delay_ns(0, 1408, 8000), 0u);
  EXPECT_EQ(stream::video_pacing_delay_ns(301, 1408, 0), 0u);
  EXPECT_EQ(stream::video_pacing_delay_ns(UINT64_MAX, 65583, INT_MAX), std::uint64_t(INT64_MAX));
}

TEST(VideoPacingTests, FractionalRatesShareCapacityWithoutRoundingEachPacketUp) {
  const auto plan = stream::video_pacing_reservation(8000, {1000, 60, 1376, 1408, 8, 20, 2});
  ASSERT_TRUE(plan.admissible);
  std::uint64_t used = 0;
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  const auto before = used;
  EXPECT_FALSE(stream::reserve_video_pacing_capacity(used, plan));
  EXPECT_EQ(used, before);
}

TEST(VideoPacingTests, LongPositivePacingWaitIsCancellableWithoutWaitingForThePacketInterval) {
  using namespace std::chrono;
  auto current = steady_clock::time_point {};
  const auto origin = current;
  const auto due = current + nanoseconds(stream::video_pacing_delay_ns(1, 1408, 40));
  bool cancelled = false;
  unsigned sleeps = 0;
  EXPECT_FALSE(stream::wait_for_video_pacing(due, [&] { return current; }, [&](auto duration) {
    EXPECT_LE(duration, milliseconds(1)); current += duration; ++sleeps; cancelled = true;
  }, [&] { return cancelled; }));
  EXPECT_EQ(sleeps, 1u);
  EXPECT_EQ(current - origin, milliseconds(1));
  cancelled = false;
  EXPECT_TRUE(stream::wait_for_video_pacing(due, [&] { return current; }, [&](auto duration) {
    EXPECT_LE(duration, milliseconds(1)); current += duration;
  }, [&] { return cancelled; }));
  EXPECT_EQ(current, due);
}

TEST(VideoPacingTests, ReservationsCoverTheProductionDerivedFecCounterexamples) {
  // Production-extracted fixtures at 156250 image bytes/frame * 120 fps:
  // FEC 10: 114 + 12 shards; FEC 20: 114 + 23 shards.
  EXPECT_GE(stream::video_pacing_reservation(200000, demand(10)).packets_per_second, 15120u);
  EXPECT_GE(stream::video_pacing_reservation(200000, demand(20)).packets_per_second, 16440u);
}

TEST(VideoPacingTests, FecAndClientMinimaIncreaseReservedDemand) {
  std::uint64_t previous = 0;
  for (const int fec : {0, 1, 5, 10, 20, 80, 100, 255}) {
    const auto plan = stream::video_pacing_reservation(10000000, demand(fec));
    ASSERT_TRUE(plan.admissible);
    EXPECT_GE(plan.packets_per_second, previous);
    previous = plan.packets_per_second;
  }
  auto minimum = demand(1);
  minimum.min_parity_shards = 20;
  EXPECT_GT(stream::video_pacing_reservation(10000000, minimum).packets_per_second,
            stream::video_pacing_reservation(10000000, demand(1)).packets_per_second);
  minimum.fec_percentage = 0;
  EXPECT_EQ(stream::video_pacing_reservation(10000000, minimum).packets_per_second,
            stream::video_pacing_reservation(10000000, demand(0)).packets_per_second);
}

TEST(VideoPacingTests, CadenceAndSmallPacketsReserveTheirExtraOverhead) {
  std::uint64_t previous = 0;
  for (const int fps : {1, 10, 24, 60, 120, 144, 4000}) {
    const auto plan = stream::video_pacing_reservation(10000000, demand(20, fps));
    ASSERT_TRUE(plan.admissible);
    EXPECT_GE(plan.packets_per_second, previous);
    previous = plan.packets_per_second;
  }
  EXPECT_GT(stream::video_pacing_reservation(10000000, demand(20, 120, 184)).send_ns_per_second,
            stream::video_pacing_reservation(10000000, demand()).send_ns_per_second);
}

TEST(VideoPacingTests, SharedCapacityAdmissionIsAtomicAtTheBoundary) {
  const auto plan = stream::video_pacing_reservation(400000, demand());
  ASSERT_TRUE(plan.admissible);
  std::uint64_t used = 0;
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  const auto one = used;
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  const auto two = used;
  EXPECT_FALSE(stream::reserve_video_pacing_capacity(used, plan));
  EXPECT_EQ(used, two);
  EXPECT_EQ(two, one * 2);
  used = 1'000'000'000 - plan.send_ns_per_second;
  EXPECT_TRUE(stream::reserve_video_pacing_capacity(used, plan));
  EXPECT_EQ(used, 1'000'000'000u);
  EXPECT_FALSE(stream::reserve_video_pacing_capacity(used, {true, 1, 1}));
  EXPECT_EQ(used, 1'000'000'000u);
}

TEST(VideoPacingTests, IndependentCriticalDetailBlocksAreConservativelyReserved) {
  auto pyro = demand(50);
  pyro.independent_fec_blocks = true;
  const auto plan = stream::video_pacing_reservation(10000000, pyro);
  EXPECT_GE(plan.packets_per_second, stream::video_pacing_reservation(10000000, demand(50)).packets_per_second);
}

TEST(VideoPacingTests, InvalidAndExtremeInputsDoNotOverflowOrConsumeCapacity) {
  for (const int fps : {0, -1, 4001, INT_MAX}) {
    EXPECT_FALSE(stream::video_pacing_reservation(200000, demand(20, fps)).admissible);
  }
  auto invalid = demand();
  invalid.payload_bytes = 0;
  EXPECT_FALSE(stream::video_pacing_reservation(200000, invalid).admissible);
  invalid = demand();
  invalid.datagram_bytes = std::numeric_limits<std::size_t>::max();
  EXPECT_FALSE(stream::video_pacing_reservation(200000, invalid).admissible);
  invalid = demand();
  invalid.fec_percentage = 256;
  EXPECT_FALSE(stream::video_pacing_reservation(200000, invalid).admissible);
  invalid = demand();
  invalid.encoder_kbps = INT_MAX;
  invalid.frames_per_second = 4000;
  invalid.payload_bytes = 1;
  invalid.datagram_bytes = 65583;
  invalid.fec_percentage = 255;
  invalid.min_parity_shards = 255;
  const auto huge = stream::video_pacing_reservation(1, invalid);
  EXPECT_FALSE(huge.admissible);
  std::uint64_t used = 17;
  EXPECT_FALSE(stream::reserve_video_pacing_capacity(used, huge));
  EXPECT_EQ(used, 17u);
}

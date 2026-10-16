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
  EXPECT_EQ(stream::video_pacing_packets_per_ms(1, 1408), 0u);
  EXPECT_EQ(stream::video_pacing_packets_per_ms(200000, 1408), 17u);
  EXPECT_EQ(stream::video_pacing_packets_per_ms(2000000, 1408), 177u);
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

TEST(VideoPacingTests, CountsHeadersEncryptionAndWholePacketQuantization) {
  const auto plain = demand();
  const auto encrypted = demand(20, 120, 1376, 32);
  const auto plan = stream::video_pacing_reservation(200000, plain);
  const auto quantum = (plan.packets_per_second + 999) / 1000;
  const int exact_ceiling = int(quantum * plain.datagram_bytes * 8);
  EXPECT_TRUE(stream::video_pacing_reservation(exact_ceiling, plain).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(exact_ceiling - 1, plain).admissible);
  EXPECT_FALSE(stream::video_pacing_reservation(exact_ceiling, encrypted).admissible);
  auto without_frame_header = plain;
  without_frame_header.frame_header_bytes = 0;
  EXPECT_GT(plan.packets_per_second, stream::video_pacing_reservation(200000, without_frame_header).packets_per_second);
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

#include "../tests_common.h"

#include "src/pyrowave_policy.h"
#include "src/pyrowave_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
  using namespace pyrowave::policy;
  namespace protocol = pyrowave::protocol;

  // 1392-byte packets, the Moonlight default: 1376 frame bytes per shard.
  constexpr int PACKET_SIZE = 1392;
  constexpr std::size_t SHARD = 1376;

  void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int i = 0; i < 4; i++) {
      out.push_back(std::uint8_t(value >> (8 * i)));
    }
  }

  std::uint32_t get_u32(const std::uint8_t *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
  }

  /// A synthetic PyroWave frame: sequence header, then block records of the given
  /// sizes in words, block_index = position, payload words filled with a marker.
  std::vector<std::uint8_t> make_bitstream(const std::vector<std::uint32_t> &block_words, std::uint32_t width = 1920, std::uint32_t height = 1080, bool chroma444 = false) {
    std::vector<std::uint8_t> out;
    put_u32(out, (width - 1) | ((height - 1) << 14) | (1u << 31));
    put_u32(out, std::uint32_t(block_words.size()) | (chroma444 ? 1u << 26 : 0u));
    for (std::uint32_t index = 0; index < block_words.size(); index++) {
      const auto words = block_words[index];
      put_u32(out, 0x5a5au | (words << 16));
      put_u32(out, 7u | (index << 8));
      for (std::uint32_t w = 2; w < words; w++) {
        put_u32(out, (index << 12) | w);
      }
    }
    return out;
  }

  /// Block records of a frame keyed by block_index, each as its raw bytes.
  std::map<std::uint32_t, std::vector<std::uint8_t>> records_by_index(const std::vector<std::uint8_t> &stripped) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> records;
    for (std::size_t position = 8; position < stripped.size();) {
      const auto words = (get_u32(stripped.data() + position) >> 16) & 0xfff;
      const auto index = get_u32(stripped.data() + position + 4) >> 8;
      records[index].assign(stripped.begin() + position, stripped.begin() + position + words * 4);
      position += words * 4;
    }
    return records;
  }

  std::vector<std::uint32_t> random_block_words(std::size_t count, std::uint32_t seed) {
    // Roughly the spread of a real 1080p frame: mostly small blocks, a few large ones.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::uint32_t> small(2, 120);
    std::uniform_int_distribution<std::uint32_t> large(121, 600);
    std::uniform_int_distribution<int> pick(0, 9);
    std::vector<std::uint32_t> words(count);
    for (auto &w : words) {
      w = pick(rng) == 0 ? large(rng) : small(rng);
    }
    return words;
  }
}  // namespace

TEST(PyroWavePolicy, SelectsRecordFramingForAwareClients) {
  EXPECT_EQ(select_framing(true, std::nullopt), framing_e::records);
  EXPECT_EQ(select_framing(true, 0u), framing_e::records);
  EXPECT_EQ(select_framing(false, protocol::FEATURE_RECORD_FRAMING), framing_e::records);
  // 0x2 was once reserved for partial-frame decoding and is ignored.
  EXPECT_EQ(select_framing(false, protocol::FEATURE_RECORD_FRAMING | 0x2u), framing_e::records);
  EXPECT_EQ(select_framing(false, 0x2u), framing_e::length_prefixed);
  EXPECT_EQ(select_framing(false, 0u), framing_e::length_prefixed);
  EXPECT_EQ(select_framing(false, std::nullopt), framing_e::length_prefixed);
}

TEST(PyroWavePolicy, ShardPayloadMatchesTheRtpLayer) {
  EXPECT_EQ(shard_payload_bytes(PACKET_SIZE), SHARD);
  EXPECT_EQ(shard_payload_bytes(1024), 1008u);
  EXPECT_EQ(shard_payload_bytes(0), 0u);
  EXPECT_EQ(shard_payload_bytes(16), 0u);
  EXPECT_EQ(shard_payload_bytes(36), 0u);  // cannot fit the headers and padding
  EXPECT_EQ(shard_payload_bytes(64), 48u);
  EXPECT_EQ(shard_payload_bytes(1390), 0u);  // not a whole number of words
}

TEST(PyroWavePolicy, RecordFrameRoundTripsAndAlignsToShards) {
  for (std::uint32_t seed = 1; seed <= 20; seed++) {
    const auto words = random_block_words(2000, seed);
    const auto bitstream = make_bitstream(words);

    std::vector<std::uint8_t> frame;
    const auto stats = write_record_frame(bitstream, SHARD, frame);
    ASSERT_TRUE(stats) << "seed " << seed;
    EXPECT_EQ(stats->block_records, words.size());

    const auto info = inspect_record_frame(frame, SHARD);
    ASSERT_TRUE(info.valid) << info.error;
    EXPECT_EQ(info.width, 1920u);
    EXPECT_EQ(info.height, 1080u);
    EXPECT_FALSE(info.chroma444);
    // A receiver requires total_blocks == the number of block records.
    EXPECT_EQ(info.total_blocks, words.size());
    EXPECT_EQ(info.block_records, words.size());
    EXPECT_EQ(info.padding_records, stats->padding_records);
    EXPECT_EQ(info.padding_bytes, stats->padding_bytes);
    EXPECT_EQ(frame.size(), bitstream.size() + stats->padding_bytes);

    // Every shard after the oversized records starts with a record, and the
    // coarsest level precedes finer blocks: what partial decoding relies on.
    EXPECT_EQ(info.misaligned_records, 0u) << "seed " << seed;
    EXPECT_EQ(info.late_oversized_records, 0u) << "seed " << seed;
    EXPECT_EQ(info.late_coarse_records, 0u) << "seed " << seed;
    EXPECT_EQ(info.oversized_records, stats->oversized_records) << "seed " << seed;
    EXPECT_EQ(info.critical_bytes, stats->critical_bytes) << "seed " << seed;
    // First-fit keeps padding small (strict order costs about 20% here).
    EXPECT_LT(stats->padding_bytes * 100, frame.size() * 2) << "seed " << seed;

    // Same records, same bytes, regardless of order.
    EXPECT_EQ(records_by_index(info.stripped), records_by_index(bitstream));
    // The sequence header stays first and unchanged.
    EXPECT_TRUE(std::equal(bitstream.begin(), bitstream.begin() + 8, frame.begin()));
  }
}

TEST(PyroWavePolicy, RecordFrameFillsShardGapsWithLaterRecords) {
  // A 1000-word record cannot follow the first 300-word record in the first shard;
  // the later 40-word records fill that gap instead of padding.
  const std::vector<std::uint32_t> words {300, 300, 300, 200, 40, 40, 40, 40, 40};
  const auto bitstream = make_bitstream(words);
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, SHARD, frame);
  ASSERT_TRUE(stats);
  EXPECT_GT(stats->reordered_records, 0u);
  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.block_records, words.size());
  EXPECT_EQ(info.misaligned_records, 0u);
}

TEST(PyroWavePolicy, RecordFramePadsWhenNothingFits) {
  // Two 1000-byte records cannot share a 1376-byte shard: pad after the first.
  const auto bitstream = make_bitstream({250, 250});
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, SHARD, frame);
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->padding_records, 1u);
  // First shard: 8-byte frame header + 8-byte sequence header + 1000 bytes + padding.
  EXPECT_EQ(stats->padding_bytes, SHARD - protocol::FRAME_HEADER_BYTES - 8 - 1000);
  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.misaligned_records, 0u);
  EXPECT_EQ(info.padding_records, 1u);
}

TEST(PyroWavePolicy, RecordFrameLetsOversizedRecordsSpanShards) {
  const auto bitstream = make_bitstream({1000, 30, 1000});
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, SHARD, frame);
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->oversized_records, 2u);
  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.oversized_records, 2u);
  EXPECT_EQ(info.block_records, 3u);
  // Both go ahead of the ordinary record between them
  EXPECT_EQ(info.late_oversized_records, 0u);
  EXPECT_EQ(info.out_of_order_records, 1u);
  EXPECT_EQ(records_by_index(info.stripped), records_by_index(bitstream));
}

TEST(PyroWavePolicy, CoarseBlockCountMatchesTheWaveletLayout) {
  // Four bands of three components at the coarsest of five levels, on dimensions
  // padded to 32 pixels (at least 128).
  EXPECT_EQ(coarse_block_count(1920, 1080), 2u * 2u * 12u);
  EXPECT_EQ(coarse_block_count(1280, 720), 2u * 1u * 12u);
  EXPECT_EQ(coarse_block_count(3840, 2160), 4u * 3u * 12u);
  EXPECT_EQ(coarse_block_count(64, 64), 12u);
}

TEST(PyroWavePolicy, RecordFrameSendsTheCoarsestLevelFirst) {
  // 1080p has 48 coarsest-level blocks. Large coarse records leave gaps that the
  // small finer records would fill first-fit; they must wait for the coarse ones.
  std::vector<std::uint32_t> words(60, 10);
  std::fill(words.begin(), words.begin() + 48, 200);
  const auto bitstream = make_bitstream(words);
  std::vector<std::uint8_t> frame;
  ASSERT_TRUE(write_record_frame(bitstream, SHARD, frame));
  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.late_coarse_records, 0u);
  EXPECT_EQ(info.misaligned_records, 0u);
  EXPECT_EQ(records_by_index(info.stripped), records_by_index(bitstream));
}

TEST(PyroWavePolicy, RecordFrameReportsTheCriticalPrefix) {
  // Coarse records of every size come first, oversized ones leading; the finer
  // level's oversized records then precede its ordinary ones.
  std::vector<std::uint32_t> words(52, 10);
  words[3] = 1000;  // coarse, oversized
  words[50] = 1000;  // finer, oversized
  const auto bitstream = make_bitstream(words);
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, SHARD, frame);
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->oversized_records, 2u);

  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.late_coarse_records, 0u);
  EXPECT_EQ(info.late_oversized_records, 0u);
  EXPECT_EQ(info.misaligned_records, 0u);
  EXPECT_EQ(info.critical_bytes, stats->critical_bytes);
  // The sequence header, the oversized coarse record, 47 small ones and padding
  EXPECT_GE(stats->critical_bytes, 8u + 4000u + 47u * 40u);
  EXPECT_LT(stats->critical_bytes, 8u + 4000u + 47u * 40u + SHARD);
  // The oversized coarse record leads, and the finer oversized one follows the prefix
  EXPECT_EQ(get_u32(frame.data() + 8 + 4) >> 8, 3u);
  std::size_t position = stats->critical_bytes;
  while (get_u32(frame.data() + position) == protocol::PADDING_MAGIC) {
    position += 8 + 4 * std::size_t(get_u32(frame.data() + position + 4));
  }
  EXPECT_EQ(get_u32(frame.data() + position + 4) >> 8, 50u);
}

TEST(PyroWavePolicy, RecordStartShardsMarkWhereAReceiverCanResume) {
  // 64-byte shards; the first holds 56 frame bytes after the frame header.
  //   shard 0 [0, 56):    sequence header, 48-byte block
  //   shard 1 [56, 120):  56-byte block, 8-byte padding
  //   shard 2 [120, 184): two 32-byte blocks
  //   shard 3-4 [184, 304): 120-byte block, then padding up to 312
  //   shard 5 [312, 368): 56-byte block
  std::vector<std::uint8_t> frame = make_bitstream({});
  const auto put_block = [&](std::uint32_t index, std::uint32_t words) {
    put_u32(frame, 0x5a5au | (words << 16));
    put_u32(frame, 7u | (index << 8));
    for (std::uint32_t w = 2; w < words; w++) {
      put_u32(frame, w);
    }
  };
  const auto put_padding = [&](std::uint32_t bytes) {
    put_u32(frame, protocol::PADDING_MAGIC);
    put_u32(frame, bytes / 4 - 2);
    frame.resize(frame.size() + bytes - 8, 0);
  };
  put_block(0, 12);
  put_block(1, 14);
  put_padding(8);
  put_block(2, 8);
  put_block(3, 8);
  put_block(4, 30);
  put_padding(8);
  put_block(5, 14);
  ASSERT_EQ(frame.size(), 368u);

  const std::vector<bool> expected {true, true, true, true, false, true};
  EXPECT_EQ(record_start_shards(frame, 64), expected);

  // A frame that cannot be walked gets no flags rather than wrong ones
  frame.resize(frame.size() - 4);
  EXPECT_TRUE(record_start_shards(frame, 64).empty());
  EXPECT_TRUE(record_start_shards(frame, 0).empty());
}

TEST(PyroWavePolicy, ParityShardsMatchTheFecEncoder) {
  EXPECT_EQ(parity_shards(21, 20, 2), 5u);
  EXPECT_EQ(parity_shards(7, 20, 2), 2u);
  EXPECT_EQ(parity_shards(1, 20, 2), 2u);
  EXPECT_EQ(parity_shards(10, 100, 2), 10u);
  EXPECT_EQ(parity_shards(21, 0, 2), 0u);
}

TEST(PyroWavePolicy, FecPlanProtectsOnlyTheCriticalShards) {
  const auto plan = plan_fec_blocks(400, 21, 20, 2);
  ASSERT_EQ(plan.size(), 3u);
  EXPECT_EQ(plan[0].data_shards, 21u);
  EXPECT_EQ(plan[0].fec_percentage, 20);
  EXPECT_EQ(plan[1].data_shards, 190u);
  EXPECT_EQ(plan[1].fec_percentage, 0);
  EXPECT_EQ(plan[2].data_shards, 189u);
  EXPECT_EQ(plan[2].fec_percentage, 0);

  // A frame that is all critical is one protected block
  const auto tiny = plan_fec_blocks(5, 9, 20, 2);
  ASSERT_EQ(tiny.size(), 1u);
  EXPECT_EQ(tiny[0].data_shards, 5u);
  EXPECT_EQ(tiny[0].fec_percentage, 20);

  // The largest frame the encoder budget allows still fits three unprotected blocks
  const auto large = plan_fec_blocks(3000 + 30, 30, 20, 2);
  ASSERT_EQ(large.size(), 4u);
  for (std::size_t i = 1; i < large.size(); i++) {
    EXPECT_LE(large[i].data_shards, MAX_FEC_BLOCK_SHARDS);
  }
}

TEST(PyroWavePolicy, FecPlanFallsBackToNoParity) {
  const auto expect_unprotected = [](const std::vector<fec_block_t> &plan, std::size_t total, std::size_t blocks) {
    ASSERT_EQ(plan.size(), blocks);
    std::size_t sum = 0;
    for (const auto &block : plan) {
      EXPECT_EQ(block.fec_percentage, 0);
      sum += block.data_shards;
    }
    EXPECT_EQ(sum, total);
  };

  // Disabled, nothing critical, or too large for one Reed-Solomon block
  expect_unprotected(plan_fec_blocks(400, 21, 0, 2), 400, 2);
  expect_unprotected(plan_fec_blocks(400, 0, 20, 2), 400, 2);
  expect_unprotected(plan_fec_blocks(400, 230, 20, 2), 400, 2);
  expect_unprotected(plan_fec_blocks(3900, 0, 20, 2), 3900, 4);
  EXPECT_TRUE(plan_fec_blocks(0, 0, 20, 2).empty());
}

TEST(PyroWavePolicy, DetailFecProtectsSmallFramesAndPartOfLargeFrames) {
  const auto small = plan_fec_blocks(400, 21, 20, 2, 20, 100);
  ASSERT_EQ(small.size(), 3u);
  EXPECT_EQ(small.front().data_shards, 21u);
  for (const auto &block : small) {
    EXPECT_EQ(block.fec_percentage, 20);
    EXPECT_LE(block.data_shards + parity_shards(block.data_shards, block.fec_percentage, 2), 255u);
  }

  const auto large = plan_fec_blocks(3000, 30, 20, 2, 20, 100);
  ASSERT_EQ(large.size(), 4u);
  EXPECT_GT(large.front().data_shards, 30u);
  EXPECT_EQ(large.front().fec_percentage, 20);
  EXPECT_EQ(large.back().fec_percentage, 0);

  // With little spare bandwidth, cover all detail at a lower percentage.
  const auto tight = plan_fec_blocks(400, 21, 20, 2, 20, 4);
  ASSERT_EQ(tight.size(), 3u);
  EXPECT_EQ(tight[1].fec_percentage, 1);
  EXPECT_EQ(tight[2].fec_percentage, 1);
  EXPECT_EQ(plan_fec_blocks(400, 21, 20, 2, 20, 0)[1].fec_percentage, 0);
  EXPECT_EQ(plan_fec_blocks(400, 21, 0, 2, 20, 100)[0].fec_percentage, 0);
}

TEST(PyroWavePolicy, DetailFecAlwaysHonorsTransportAndParityBudgets) {
  std::mt19937 rng(0xfec);
  for (int attempt = 0; attempt < 4000; ++attempt) {
    const std::size_t total = 1 + rng() % 3000;
    const std::size_t critical = std::min<std::size_t>(total, 1 + rng() % 255);
    const int critical_rate = 1 + rng() % 255;
    const int detail_rate = 1 + rng() % 75;  // Also exercise requests above the 50% cap.
    const std::size_t minimum = 2 + rng() % 8;
    const std::size_t budget = rng() % 100;
    const auto baseline = plan_fec_blocks(total, critical, critical_rate, minimum);
    const auto plan = plan_fec_blocks(total, critical, critical_rate, minimum, detail_rate, budget);
    SCOPED_TRACE(attempt);
    ASSERT_LE(plan.size(), 4u);
    std::size_t data = 0;
    std::size_t parity = 0;
    std::size_t base_parity = 0;
    for (const auto &block : baseline) {
      base_parity += parity_shards(block.data_shards, block.fec_percentage, minimum);
    }
    for (const auto &block : plan) {
      EXPECT_GT(block.data_shards, 0u);
      EXPECT_LE(block.data_shards, MAX_FEC_BLOCK_SHARDS);
      const auto extra = parity_shards(block.data_shards, block.fec_percentage, minimum);
      if (extra) {
        EXPECT_LE(block.data_shards + extra, MAX_REED_SOLOMON_SHARDS);
      }
      data += block.data_shards;
      parity += extra;
    }
    EXPECT_EQ(data, total);
    EXPECT_GE(parity, base_parity);
    EXPECT_LE(parity, base_parity + budget);
    if (baseline.front().fec_percentage) {
      EXPECT_GE(plan.front().data_shards, critical);
      EXPECT_EQ(plan.front().fec_percentage, critical_rate);
    }
  }
}

TEST(PyroWavePolicy, DetailFecCanReachFiftyPercentWithoutChangingCriticalProtection) {
  for (const int requested : {50, 75, 100}) {
    const auto plan = plan_fec_blocks(400, 21, 20, 2, requested, 1000);
    ASSERT_EQ(plan.size(), 4u);
    EXPECT_EQ(plan.front().data_shards, 21u);
    EXPECT_EQ(plan.front().fec_percentage, 20);
    std::size_t data = plan.front().data_shards;
    std::size_t detail_parity = 0;
    for (std::size_t i = 1; i < plan.size(); ++i) {
      EXPECT_EQ(plan[i].fec_percentage, 50);
      const auto parity = parity_shards(plan[i].data_shards, plan[i].fec_percentage, 2);
      EXPECT_LE(plan[i].data_shards + parity, MAX_REED_SOLOMON_SHARDS);
      data += plan[i].data_shards;
      detail_parity += parity;
    }
    EXPECT_EQ(data, 400u);
    EXPECT_LE(detail_parity, parity_shards(379, 50, 0));
  }
  // A ~0.83 MB frame has too much detail for three 50% FEC blocks.
  // Lower the rate to cover all detail while keeping the critical block intact.
  const auto large = plan_fec_blocks(606, 21, 20, 2, 50, 1000);
  ASSERT_EQ(large.size(), 4u);
  EXPECT_EQ(large.front().data_shards, 21u);
  EXPECT_EQ(large.front().fec_percentage, 20);
  for (std::size_t i = 1; i < large.size(); ++i) {
    EXPECT_EQ(large[i].data_shards, 195u);
    EXPECT_EQ(large[i].fec_percentage, 30);
  }
}

TEST(PyroWavePolicy, DetailFecCadenceTargetsMatchTheAgreedRanges) {
  using namespace std::chrono;
  const auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  struct target_t {
    int fps;
    int parity;
  };
  for (const auto [fps, parity] : {target_t {120, 0}, {110, 9}, {100, 20}, {90, 33}, {80, 50}, {60, 50}, {30, 50}}) {
    SCOPED_TRACE(fps);
    detail_fec_controller_t controller(120);
    auto now = steady_clock::time_point {};
    const auto interval = duration_cast<steady_clock::duration>(duration<double>(1.0 / fps));
    detail_fec_t result;
    for (int i = 0; i < fps; ++i) {
      result = controller.observe(frame, now, 800000);
      now += interval;
    }
    // Cadence timestamps and integer FEC percentages round down.
    EXPECT_GE(result.percentage, std::max(0, parity - 1));
    EXPECT_LE(result.percentage, parity);
    EXPECT_LE(result.frame_wire_budget, std::size_t(800000.0 * 125.0 / fps));
  }
}

TEST(PyroWavePolicy, DetailFecRequiresSustainedLowCadenceAndSimilarity) {
  using namespace std::chrono;
  const auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  detail_fec_controller_t slow(120), fast(120);
  auto slow_time = steady_clock::time_point {};
  auto fast_time = slow_time;
  for (int i = 0; i < 30; ++i) {
    const auto result = slow.observe(frame, slow_time, 100000);
    if (i < 8) {
      EXPECT_EQ(result.percentage, 0);
    } else {
      EXPECT_EQ(result.percentage, 50);
      EXPECT_LE(result.frame_wire_budget, 100000u * 125 / 30);
    }
    EXPECT_EQ(fast.observe(frame, fast_time, 100000).percentage, 0);
    slow_time += microseconds(33333);
    fast_time += microseconds(8333);
  }
  // Immediate exit on a return to high cadence; no averaging tail of extra FEC.
  EXPECT_EQ(slow.observe(frame, slow_time - microseconds(25000), 100000).percentage, 0);
}

TEST(PyroWavePolicy, DetailFecTracksNegotiatedCadenceInsteadOfAnAbsoluteFpsThreshold) {
  using namespace std::chrono;
  const auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  for (const auto target : {30, 60, 120}) {
    detail_fec_controller_t controller(target);
    auto now = steady_clock::time_point {};
    const auto nominal = duration_cast<steady_clock::duration>(duration<double>(1.0 / target));
    const auto slower = duration_cast<steady_clock::duration>(duration<double>(1.10 / target));
    // Normal cadence, including a native 30 FPS stream, does not request FEC.
    for (int i = 0; i < target; ++i) {
      EXPECT_EQ(controller.observe(frame, now, 800000).percentage, 0);
      now += nominal;
    }
    detail_fec_t result;
    for (int i = 0; i < 2 * target; ++i) {
      now += slower;
      result = controller.observe(frame, now, 800000);
    }
    EXPECT_GE(result.percentage, 9);
    EXPECT_LE(result.percentage, 10);
    now += nominal;
    EXPECT_EQ(controller.observe(frame, now, 800000).percentage, 0);
  }
  EXPECT_THROW(detail_fec_controller_t(0), std::invalid_argument);
}

TEST(PyroWavePolicy, DetailFecIgnoresSequenceAndRecordOrderButDetectsMotion) {
  using namespace std::chrono;
  detail_fec_controller_t controller(120);
  auto now = steady_clock::time_point {};
  auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  for (int i = 0; i < 20; ++i) {
    auto encoded = frame;
    // Move the last record first and vary the per-frame sequence bits.
    std::rotate(encoded.begin() + 8, encoded.end() - 48, encoded.end());
    encoded[3] = (encoded[3] & 0x8f) | ((i % 8) << 4);
    for (std::size_t offset = 8; offset < encoded.size(); offset += 48) {
      encoded[offset + 3] = (encoded[offset + 3] & 0x8f) | ((i % 8) << 4);
    }
    if (i & 1) {
      // Padding must not be counted as picture activity.
      put_u32(encoded, protocol::PADDING_MAGIC);
      put_u32(encoded, 2);
      put_u32(encoded, 0);
      put_u32(encoded, 0);
    }
    const auto result = controller.observe(encoded, now, 100000);
    if (i > 8) {
      EXPECT_EQ(result.percentage, 50);
    }
    now += microseconds(33333);
  }
  // Change only one of ten equal-sized records: still a mostly stable picture.
  frame[16] ^= 1;
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 50);
  now += microseconds(33333);
  for (std::size_t offset = 8; offset < frame.size(); offset += 48) {
    frame[offset + 8] ^= 2;
  }
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
}

TEST(PyroWavePolicy, DetailFecRejectsHitchesDiscontinuitiesAndMalformedFrames) {
  using namespace std::chrono;
  detail_fec_controller_t controller(120);
  const auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  auto now = steady_clock::time_point {};
  controller.observe(frame, now, 100000);
  now += milliseconds(100);
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
  now += seconds(2);
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
  for (int i = 0; i < 20; ++i) {
    now += microseconds(33333);
    controller.observe(frame, now, 100000);
  }
  now += microseconds(33333);
  EXPECT_GT(controller.observe(frame, now, 100000).percentage, 0);
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
  now += microseconds(33333);
  auto truncated = frame;
  truncated.pop_back();
  EXPECT_EQ(controller.observe(truncated, now, 100000).percentage, 0);
  now += microseconds(33333);
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
}

TEST(PyroWavePolicy, DetailFecHandlesSparseBlocksAndDisappearingDetail) {
  using namespace std::chrono;
  detail_fec_controller_t controller(120);
  auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  // The sequence's count is the number of nonzero records, not the ID limit.
  for (std::size_t offset = 8; offset < frame.size(); offset += 48) {
    frame[offset + 6] = 1;
  }
  auto now = steady_clock::time_point {};
  for (int i = 0; i < 20; ++i) {
    const auto result = controller.observe(frame, now, 100000);
    if (i > 8) {
      EXPECT_EQ(result.percentage, 50);
    }
    now += microseconds(33333);
  }
  // One record turning to zero should not invalidate the format or all matches.
  frame.resize(frame.size() - 48);
  frame[4] = 9;
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 50);
  now += microseconds(33333);
  // Most detail disappearing is activity even if all surviving records match.
  frame.resize(8 + 48);
  frame[4] = 1;
  EXPECT_EQ(controller.observe(frame, now, 100000).percentage, 0);
}

TEST(PyroWavePolicy, RecordFrameNeverStrandsFourBytes) {
  // After the sequence header the first shard has 1360 bytes left. An oversized
  // 2732-byte record would end 4 bytes short of the third shard's end, so a
  // minimal padding record shifts it.
  const auto bitstream = make_bitstream({683, 10, 10});
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, SHARD, frame);
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->oversized_records, 1u);
  EXPECT_GE(stats->padding_records, 1u);
  const auto info = inspect_record_frame(frame, SHARD);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.misaligned_records, 0u);

  // A record of a shard less 4 bytes could never be placed alone without
  // stranding 4 bytes, so it is treated as oversized; one 4 bytes smaller is not.
  std::vector<std::uint8_t> frame2;
  const auto stats2 = write_record_frame(make_bitstream({343, 342, 342}), SHARD, frame2);
  ASSERT_TRUE(stats2);
  EXPECT_EQ(stats2->oversized_records, 1u);
  const auto info2 = inspect_record_frame(frame2, SHARD);
  ASSERT_TRUE(info2.valid) << info2.error;
  EXPECT_EQ(info2.misaligned_records, 0u);
}

TEST(PyroWavePolicy, RecordFrameWithoutAlignmentCopiesTheBitstream) {
  const auto bitstream = make_bitstream({10, 20, 30}, 3840, 2160, true);
  std::vector<std::uint8_t> frame;
  const auto stats = write_record_frame(bitstream, 0, frame);
  ASSERT_TRUE(stats);
  EXPECT_EQ(frame, bitstream);
  const auto info = inspect_record_frame(frame, 0);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_EQ(info.width, 3840u);
  EXPECT_EQ(info.height, 2160u);
  EXPECT_TRUE(info.chroma444);
}

TEST(PyroWavePolicy, RecordFrameRejectsMalformedBitstreams) {
  std::vector<std::uint8_t> frame;
  EXPECT_FALSE(write_record_frame({}, SHARD, frame));

  auto no_sequence_header = make_bitstream({10});
  no_sequence_header[3] &= 0x7f;  // clear `extended`
  EXPECT_FALSE(write_record_frame(no_sequence_header, SHARD, frame));

  auto short_block = make_bitstream({10});
  short_block[8 + 2] = 1;  // payload_words = 1
  short_block[8 + 3] = 0;
  EXPECT_FALSE(write_record_frame(short_block, SHARD, frame));

  auto truncated = make_bitstream({10});
  truncated.resize(truncated.size() - 4);
  EXPECT_FALSE(write_record_frame(truncated, SHARD, frame));

  EXPECT_TRUE(frame.empty());
}

TEST(PyroWavePolicy, InspectRejectsWhatReceiversReject) {
  EXPECT_FALSE(inspect_record_frame({}, SHARD).valid);

  auto block_first = make_bitstream({10});
  block_first.erase(block_first.begin(), block_first.begin() + 8);
  EXPECT_FALSE(inspect_record_frame(block_first, SHARD).valid);

  auto keep_previous = make_bitstream({10});
  keep_previous[7] |= 0x1;  // sequence header code 1
  EXPECT_FALSE(inspect_record_frame(keep_previous, SHARD).valid);

  auto overrun = make_bitstream({10});
  overrun.resize(overrun.size() - 4);
  EXPECT_FALSE(inspect_record_frame(overrun, SHARD).valid);

  std::vector<std::uint8_t> padding_overrun = make_bitstream({});
  put_u32(padding_overrun, protocol::PADDING_MAGIC);
  put_u32(padding_overrun, 3);
  EXPECT_FALSE(inspect_record_frame(padding_overrun, SHARD).valid);
}

TEST(PyroWavePolicy, LengthPrefixedFrameLayout) {
  const std::vector<std::uint8_t> bitstream {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  const std::vector<packet_t> packets {{0, 4}, {4, 6}};
  std::vector<std::uint8_t> frame;
  write_length_prefixed_frame(packets, bitstream.data(), frame);

  const std::vector<std::uint8_t> expected {
    2, 0, 0, 0,
    4, 0, 0, 0, 1, 2, 3, 4,
    6, 0, 0, 0, 5, 6, 7, 8, 9, 10
  };
  EXPECT_EQ(frame, expected);
  // Never mistaken for record framing: a packet count lacks the `extended` bit.
  EXPECT_EQ(frame[3] & 0x80, 0);
}

TEST(PyroWavePolicy, BudgetFollowsSubmittedFramesIncludingRepeats) {
  using namespace std::chrono_literals;

  // 600 Mbps at 120 fps: 625000 bytes per frame.
  budget_t budget(120, 600000, 0);
  EXPECT_EQ(budget.bytes_per_frame(), 625000u);

  budget.set_bitrate(300000);
  EXPECT_EQ(budget.bytes_per_frame(), 312500u);

  // At 60 submissions per second, including repeats, each gets twice the bytes.
  auto t = std::chrono::steady_clock::time_point {} + 1s;
  for (int i = 0; i < 200; i++) {
    budget.on_frame(t);
    t += 16667us;
  }
  EXPECT_NEAR(double(budget.bytes_per_frame()), 625000.0, 625000.0 * 0.01);
  EXPECT_NEAR(budget.frame_fps(), 60.0, 0.5);

  // No fixed 2x ceiling: lower submission rates get their actual share.
  budget.on_frame(t + 16666us);
  EXPECT_NEAR(double(budget.bytes_per_frame()), 1250000.0, 100.0);
  EXPECT_EQ(budget.bytes_per_frame() % 4, 0u);
}

TEST(PyroWavePolicy, BudgetHonorsLimits) {
  budget_t capped(60, 2000000, 1000000);
  EXPECT_EQ(capped.bytes_per_frame(), 1000000u);

  budget_t floor(60, 1, 0);
  EXPECT_EQ(floor.bytes_per_frame(), 0u);  // cannot fit even the codec headers

  budget_t small_cap(60, 480000, 1025);
  EXPECT_EQ(small_cap.bytes_per_frame(), 1024u);
  EXPECT_THROW(budget_t(0, 480000, 0), std::invalid_argument);
}

TEST(PyroWavePolicy, StableFrameBudgetLeavesCadenceSavingsForFec) {
  using namespace std::chrono;
  budget_t budget(120, 800000, max_bitstream_bytes(PACKET_SIZE, false, true), true);
  const auto nominal_bytes = budget.bytes_per_frame();
  EXPECT_EQ(nominal_bytes, 833332u);
  auto now = steady_clock::time_point {};
  budget.on_frame(now);
  const auto frame = make_bitstream(std::vector<std::uint32_t>(10, 12));
  detail_fec_controller_t controller(120);
  controller.observe(frame, now, 800000);
  detail_fec_t request;
  for (int i = 0; i < 60; ++i) {
    now += microseconds(16667);
    budget.on_frame(now);
    EXPECT_EQ(budget.bytes_per_frame(), nominal_bytes);
    request = controller.observe(frame, now, 800000);
  }
  EXPECT_EQ(request.percentage, 50);
  // Model a full-sized image plus a representative 21-shard critical prefix.
  const auto data = (budget.bytes_per_frame() + protocol::FRAME_HEADER_BYTES + SHARD - 1) / SHARD;
  constexpr std::size_t wire_bytes = 1500;  // Includes a conservative header allowance.
  const auto baseline = data + parity_shards(21, 20, 2);
  const auto allowance = request.frame_wire_budget / wire_bytes;
  ASSERT_GT(allowance, baseline);
  const auto plan = plan_fec_blocks(data, 21, 20, 2, request.percentage, allowance - baseline);
  std::size_t wire_packets = 0;
  for (const auto &block : plan) {
    EXPECT_GT(block.fec_percentage, 0);  // All detail now fits protected blocks.
    wire_packets += block.data_shards + parity_shards(block.data_shards, block.fec_percentage, 2);
  }
  EXPECT_LE(wire_packets * wire_bytes, request.frame_wire_budget);
  // Further FPS loss or a stall does not inflate the encoded image budget.
  now += microseconds(33333);
  budget.on_frame(now);
  EXPECT_EQ(budget.bytes_per_frame(), nominal_bytes);
  now += seconds(10);
  budget.on_frame(now);
  EXPECT_EQ(budget.bytes_per_frame(), nominal_bytes);
  budget.set_bitrate(400000);
  EXPECT_EQ(budget.bytes_per_frame(), 416664u);
  // Faster submissions still get less budget; no overspend above negotiated FPS.
  now += microseconds(4000);
  budget.on_frame(now);
  EXPECT_EQ(budget.bytes_per_frame(), 200000u);
  budget.on_frame(now);
  EXPECT_EQ(budget.bytes_per_frame(), 0u);
}

TEST(PyroWavePolicy, PausesAndDuplicateTimestampsRespectTheTransportBudget) {
  using namespace std::chrono_literals;
  budget_t budget(120, 600000, 1000000);
  const auto start = std::chrono::steady_clock::time_point {};
  budget.on_frame(start);
  budget.on_frame(start + 10s);
  EXPECT_EQ(budget.bytes_per_frame(), 1000000u);
  budget.on_frame(start + 10s);
  EXPECT_EQ(budget.bytes_per_frame(), 0u);
  budget.on_frame(start);
  EXPECT_EQ(budget.bytes_per_frame(), 0u);
  budget.on_frame(start + 10s + 8333us);
  EXPECT_NEAR(double(budget.bytes_per_frame()), 625000.0, 100.0);
}

TEST(PyroWavePolicy, CapacityUsesNegotiatedPayloadEvenWithoutRecordAlignment) {
  for (const auto packet_size : {64, 1024, 1390, 1392, 1500}) {
    const auto payload = std::size_t(packet_size - 16);
    const auto capacity = max_frame_bytes(packet_size);
    EXPECT_EQ(capacity, 4000u * payload - 8u);
    EXPECT_EQ(max_frame_bytes(packet_size, true), 3000u * payload - 8u);
    EXPECT_LE(max_bitstream_bytes(packet_size, false), capacity);
    const auto legacy_budget = max_bitstream_bytes(packet_size, true);
    EXPECT_LE(legacy_budget + (legacy_budget / 8u) * 4u + 4u, capacity);
  }
  EXPECT_EQ(max_frame_bytes(0), 0u);
  EXPECT_EQ(max_bitstream_bytes(16, true), 0u);
}

TEST(PyroWavePolicy, FramingDropsPaddingWhenItWouldExceedCapacity) {
  const auto bitstream = make_bitstream({200, 200, 200});
  std::vector<std::uint8_t> padded;
  ASSERT_TRUE(write_record_frame(bitstream, SHARD, padded));
  ASSERT_GT(padded.size(), bitstream.size());
  std::vector<std::uint8_t> frame {42};
  auto stats = write_record_frame(bitstream, SHARD, frame, bitstream.size());
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->padding_bytes, 0u);
  EXPECT_EQ(frame.front(), 42);
  EXPECT_EQ(std::vector<std::uint8_t>(frame.begin() + 1, frame.end()), bitstream);
  const auto previous = frame;
  EXPECT_FALSE(write_record_frame(bitstream, SHARD, frame, bitstream.size() - 1));
  EXPECT_EQ(frame, previous);
}

TEST(PyroWavePolicy, PacingTracksLinkCapacityAndAccountsForWireOverhead) {
  EXPECT_EQ(pacing_packets_per_ms(100000000, 600000, 1376, 1474), 8u);
  EXPECT_EQ(pacing_packets_per_ms(1000000000, 600000, 1376, 1474), 84u);
  EXPECT_EQ(pacing_packets_per_ms(2500000000, 600000, 1376, 1474), 212u);
  EXPECT_LT(pacing_packets_per_ms(1000000000, 600000, 1376, 1474 + 32),
            pacing_packets_per_ms(1000000000, 600000, 1376, 1474));
  EXPECT_EQ(pacing_packets_per_ms(0, 200000, 1376, 1474), 19u);
  EXPECT_EQ(pacing_packets_per_ms(0, 400000, 1376, 1474), 37u);
  EXPECT_EQ(pacing_packets_per_ms(0, 200000, 1376, 1474, 1376000, 120), 120u);
  EXPECT_EQ(pacing_packets_per_ms(100000000, 200000, 1376, 1474, 1376000, 120), 8u);
  EXPECT_EQ(pacing_packets_per_ms(0, 0, 0, 0), 1u);
}

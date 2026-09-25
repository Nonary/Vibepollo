#include "../tests_common.h"

#include "src/pyrowave_policy.h"
#include "src/pyrowave_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
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
  EXPECT_EQ(shard_payload_bytes(64), 0u);  // too small to be worth aligning
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

TEST(PyroWavePolicy, BudgetFollowsBitrateAndCaptureRate) {
  using namespace std::chrono_literals;

  // 600 Mbps at 120 fps: 625000 bytes per frame.
  budget_t budget(120, 600000, 0);
  EXPECT_EQ(budget.bytes_per_frame(), 625000u);

  budget.set_bitrate(300000);
  EXPECT_EQ(budget.bytes_per_frame(), 312500u);

  // A game rendering 60 fps in a 120 fps stream converges to twice the bytes.
  auto t = std::chrono::steady_clock::time_point {} + 1s;
  for (int i = 0; i < 200; i++) {
    budget.on_new_capture(t);
    t += 16667us;
  }
  EXPECT_NEAR(double(budget.bytes_per_frame()), 625000.0, 625000.0 * 0.01);
  EXPECT_NEAR(budget.capture_fps(), 60.0, 0.5);

  // A long pause never pushes past twice the nominal budget.
  budget.on_new_capture(t + 10s);
  EXPECT_LE(budget.bytes_per_frame(), 625000u + 4);
  EXPECT_EQ(budget.bytes_per_frame() % 4, 0u);
}

TEST(PyroWavePolicy, BudgetHonorsLimits) {
  budget_t capped(60, 2000000, 1000000);
  EXPECT_EQ(capped.bytes_per_frame(), 1000000u);

  budget_t floor(60, 1, 0);
  EXPECT_EQ(floor.bytes_per_frame(), 4096u);

  budget_t unknown_rate(0, 480000, 0);  // falls back to 60 fps
  EXPECT_EQ(unknown_rate.bytes_per_frame(), 1000000u);
}

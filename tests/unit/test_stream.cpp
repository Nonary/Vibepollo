/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include "../tests_common.h"
#include "src/deferred_stream_start_policy.h"
#include "src/haptics_gain.h"
#include "src/stream_protocol.h"

namespace {
  using haptics_packet_t = std::array<std::uint8_t, 960>;

  int haptics_sample(const haptics_packet_t &packet, std::size_t index) {
    const int raw = packet[index * 2] | (int(packet[index * 2 + 1]) << 8);
    return raw >= 0x8000 ? raw - 0x10000 : raw;
  }

  void set_haptics_sample(haptics_packet_t &packet, std::size_t index, int sample) {
    const auto encoded = static_cast<std::uint16_t>(sample);
    packet[index * 2] = static_cast<std::uint8_t>(encoded);
    packet[index * 2 + 1] = static_cast<std::uint8_t>(encoded >> 8);
  }

  haptics_packet_t haptics_packet(int left, int right) {
    haptics_packet_t packet {};
    for (std::size_t frame = 0; frame < packet.size() / 4; ++frame) {
      set_haptics_sample(packet, frame * 2, left);
      set_haptics_sample(packet, frame * 2 + 1, right);
    }
    return packet;
  }

  constexpr auto haptics_time = std::chrono::steady_clock::time_point {};
}  // namespace

TEST(HapticsGain, UnityPreservesEverySampleBit) {
  auto packet = haptics_packet(-32768, 32767);
  set_haptics_sample(packet, 2, -1);
  set_haptics_sample(packet, 3, 0);
  const auto original = packet;
  stream::haptics_gain_t processor;
  processor.apply(packet, 1.0, 0, haptics_time);
  EXPECT_EQ(packet, original);
}

TEST(HapticsGain, SharedLiftPreservesGapWithNearMaximumEffect) {
  for (const int sign : {1, -1}) {
    auto packet = haptics_packet(sign * 6553, sign * 29490);
    stream::haptics_gain_t processor;
    processor.apply(packet, 2.0, 0, haptics_time);
    const int left = std::abs(haptics_sample(packet, 0));
    const int right = std::abs(haptics_sample(packet, 1));
    // Approximately 20,90 -> 30,100; neither actuator crosses the other.
    EXPECT_EQ(right, sign > 0 ? 32767 : 32768);
    EXPECT_EQ(right - left, 29490 - 6553);
    EXPECT_GT(left, 6553);
  }
}

TEST(HapticsGain, ScalesWaveformShapeWithoutOffsetsOrFlatPeaks) {
  auto packet = haptics_packet(10000, 20000);
  set_haptics_sample(packet, 2, -10000);
  set_haptics_sample(packet, 3, -20000);
  set_haptics_sample(packet, 4, 5000);
  set_haptics_sample(packet, 5, 10000);
  set_haptics_sample(packet, 6, 0);
  set_haptics_sample(packet, 7, 0);
  stream::haptics_gain_t processor;
  processor.apply(packet, 1.5, 0, haptics_time);
  const std::array<int, 8> expected {15000, 25000, -15000, -25000, 7500, 12500, 0, 0};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(haptics_sample(packet, index), expected[index]);
  }
}

TEST(HapticsGain, FullScaleLeavesBothEffectsUnchanged) {
  for (const int limit : {-32768, 32767}) {
    auto packet = haptics_packet(10000, limit);
    const auto original = packet;
    stream::haptics_gain_t processor;
    processor.apply(packet, 4.0, 0, haptics_time);
    EXPECT_EQ(packet, original);
  }
}

TEST(HapticsGain, SilenceDoesNotBlockOtherActuatorOrCreateVibration) {
  auto packet = haptics_packet(0, 10000);
  stream::haptics_gain_t processor;
  processor.apply(packet, 2.0, 0, haptics_time);
  EXPECT_EQ(haptics_sample(packet, 0), 0);
  EXPECT_EQ(haptics_sample(packet, 1), 20000);
  auto silence = haptics_packet(0, 0);
  processor.apply(silence, 4.0, 1, haptics_time + std::chrono::milliseconds {5});
  EXPECT_EQ(silence, haptics_packet(0, 0));
}

TEST(HapticsGain, TinySignalsNeverExceedRequestedAmplification) {
  auto packet = haptics_packet(1, 30000);
  stream::haptics_gain_t processor;
  processor.apply(packet, 4.0, 0, haptics_time);
  EXPECT_EQ(haptics_sample(packet, 0), 4);
  EXPECT_EQ(haptics_sample(packet, 1), 30003);
}

TEST(HapticsGain, AttenuationAndMuteStayLinear) {
  auto packet = haptics_packet(1000, -1000);
  stream::haptics_gain_t processor;
  processor.apply(packet, 0.5, 0, haptics_time);
  EXPECT_EQ(haptics_sample(packet, 0), 500);
  EXPECT_EQ(haptics_sample(packet, 1), -500);
  processor.apply(packet, 0.0, 1, haptics_time);
  EXPECT_EQ(packet, haptics_packet(0, 0));
}

TEST(HapticsGain, EasesRecoveryAndImmediatelyRespectsNewHeadroom) {
  stream::haptics_gain_t processor;
  auto full = haptics_packet(10000, 32767);
  processor.apply(full, 2.0, 0, haptics_time);
  auto recovery = haptics_packet(10000, 20000);
  processor.apply(recovery, 2.0, 1, haptics_time + std::chrono::milliseconds {5});
  const int first = haptics_sample(recovery, 0);
  EXPECT_EQ(first, 10000);
  EXPECT_LT(first, 20000);
  EXPECT_EQ(haptics_sample(recovery, 1) - first, 10000);
  for (std::uint32_t sequence = 2; sequence <= 60; ++sequence) {
    recovery = haptics_packet(10000, 20000);
    processor.apply(recovery, 2.0, sequence, haptics_time + std::chrono::milliseconds {5 * sequence});
  }
  EXPECT_GT(haptics_sample(recovery, 0), first);
  auto impact = haptics_packet(10000, 30000);
  processor.apply(impact, 2.0, 61, haptics_time + std::chrono::milliseconds {305});
  EXPECT_EQ(haptics_sample(impact, 0), 12767);
  EXPECT_EQ(haptics_sample(impact, 1), 32767);
}

TEST(HapticsGain, RecentImpactKeepsSameLiftForFollowingWeakEffect) {
  stream::haptics_gain_t processor;
  auto strong = haptics_packet(29490, 29490);
  processor.apply(strong, 2.0, 0, haptics_time);
  const int strong_output = haptics_sample(strong, 0);
  auto weak = haptics_packet(6553, 6553);
  processor.apply(weak, 2.0, 1, haptics_time + std::chrono::milliseconds {5});
  EXPECT_EQ(strong_output - haptics_sample(weak, 0), 29490 - 6553);
  EXPECT_EQ(haptics_sample(weak, 0), 9830);
}

TEST(HapticsGain, BriefSilenceRetainsContrastWithPreviousEffect) {
  stream::haptics_gain_t processor;
  auto strong = haptics_packet(29490, 29490);
  processor.apply(strong, 2.0, 0, haptics_time);
  auto silence = haptics_packet(0, 0);
  processor.apply(silence, 2.0, 1, haptics_time + std::chrono::milliseconds {5});
  EXPECT_EQ(silence, haptics_packet(0, 0));
  auto weak = haptics_packet(6553, 6553);
  processor.apply(weak, 2.0, 2, haptics_time + std::chrono::milliseconds {10});
  EXPECT_EQ(haptics_sample(weak, 0), 9830);
  EXPECT_EQ(haptics_sample(strong, 0) - haptics_sample(weak, 0), 29490 - 6553);
}

TEST(HapticsGain, PeakHoldReleasesGraduallyInsteadOfSuddenlyBoostingQuietEffect) {
  stream::haptics_gain_t processor;
  auto strong = haptics_packet(29490, 29490);
  processor.apply(strong, 2.0, 0, haptics_time);
  int last_output = 0;
  for (std::uint32_t sequence = 1; sequence <= 20; ++sequence) {
    auto weak = haptics_packet(6553, 6553);
    processor.apply(weak, 2.0, sequence, haptics_time + std::chrono::milliseconds {sequence * 5});
    EXPECT_EQ(haptics_sample(weak, 0), 9830);
    last_output = haptics_sample(weak, 0);
  }
  auto released = haptics_packet(6553, 6553);
  processor.apply(released, 2.0, 21, haptics_time + std::chrono::milliseconds {105});
  EXPECT_GT(haptics_sample(released, 0), last_output);
  EXPECT_LT(haptics_sample(released, 0), 13106);
}

TEST(HapticsGain, GapsSilenceAndSettingsChangesResetHistory) {
  for (const int reset_case : {0, 1, 2, 3, 4}) {
    stream::haptics_gain_t processor;
    auto full = haptics_packet(10000, 32767);
    processor.apply(full, 2.0, 0, haptics_time);
    if (reset_case == 2) {
      auto silence = haptics_packet(0, 0);
      for (std::uint32_t sequence = 1; sequence <= 21; ++sequence) {
        processor.apply(silence, 2.0, sequence, haptics_time + std::chrono::milliseconds {sequence * 5});
      }
    }
    auto packet = haptics_packet(10000, 20000);
    const auto sequence = reset_case == 0 ? 10u : reset_case == 2 ? 22u : 1u;
    const auto time = haptics_time + std::chrono::milliseconds {reset_case == 1 ? 60 : reset_case == 2 ? 110 : reset_case == 4 ? -5 : 10};
    const double strength = reset_case == 3 ? 1.5 : 2.0;
    processor.apply(packet, strength, sequence, time);
    EXPECT_EQ(haptics_sample(packet, 0), reset_case == 3 ? 15000 : 20000);
    EXPECT_EQ(haptics_sample(packet, 1), reset_case == 3 ? 25000 : 30000);
  }
}

TEST(HapticsGain, SequenceWrapKeepsSmoothingAndControllersHaveIndependentHistory) {
  stream::haptics_gain_t first;
  stream::haptics_gain_t second;
  auto full = haptics_packet(10000, 32767);
  first.apply(full, 2.0, UINT32_MAX, haptics_time);
  auto one = haptics_packet(10000, 20000);
  auto two = one;
  first.apply(one, 2.0, 0, haptics_time + std::chrono::milliseconds {5});
  second.apply(two, 2.0, 0, haptics_time + std::chrono::milliseconds {5});
  EXPECT_LT(haptics_sample(one, 0), haptics_sample(two, 0));
  EXPECT_EQ(haptics_sample(two, 0), 20000);
}

TEST(HapticsGain, FullSignedRangePreservesSignAndPeakStrengthGap) {
  for (const double strength : {1.1, 1.5, 2.0, 4.0}) {
    for (int left = -32768; left <= 32767; ++left) {
      // A fresh packet pair exercises every signed input against a loud effect.
      std::array<std::uint8_t, 4> bytes {};
      const auto encoded = static_cast<std::uint16_t>(left);
      bytes[0] = static_cast<std::uint8_t>(encoded);
      bytes[1] = static_cast<std::uint8_t>(encoded >> 8);
      bytes[2] = 0x30;
      bytes[3] = 0x75;  // 30000
      stream::haptics_gain_t processor;
      processor.apply(bytes, strength, 0, haptics_time);
      const int raw_left = bytes[0] | (int(bytes[1]) << 8);
      const int output_left = raw_left >= 0x8000 ? raw_left - 0x10000 : raw_left;
      const int output_right = bytes[2] | (int(bytes[3]) << 8);
      ASSERT_GE(output_right, 30000);
      ASSERT_LE(output_right, 32767);
      if (left == 0) {
        ASSERT_EQ(output_left, 0);
        continue;
      }
      ASSERT_EQ(output_left < 0, left < 0);
      ASSERT_GE(std::abs(output_left), std::abs(left));
      ASSERT_LE(std::abs(output_left), std::ceil(std::abs(left) * strength));
      const double limit = left < 0 ? 32768.0 : 32767.0;
      const double original_gap = 30000 / 32767.0 - std::abs(left) / limit;
      const double output_gap = output_right / 32767.0 - std::abs(output_left) / limit;
      ASSERT_NEAR(output_gap, original_gap, 1.0 / 32767.0);
    }
  }
}

TEST(HapticsGain, InvalidInputsLeaveSamplesUnchanged) {
  for (const double strength : {-1.0, 5.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    auto packet = haptics_packet(10000, -20000);
    const auto original = packet;
    stream::haptics_gain_t processor;
    processor.apply(packet, strength, 0, haptics_time);
    EXPECT_EQ(packet, original);
  }
  std::array<std::uint8_t, 3> malformed {1, 2, 3};
  const auto original = malformed;
  stream::haptics_gain_t processor;
  processor.apply(malformed, 2.0, 0, haptics_time);
  EXPECT_EQ(malformed, original);
}
namespace {
  class lifecycle_mutex_stub_t {
  public:
    explicit lifecycle_mutex_stub_t(const bool available):
        available_ {available} {
    }

    void lock() {
      ++blocking_lock_calls;
      locked = true;
    }

    bool try_lock() {
      ++try_lock_calls;
      if (!available_) {
        return false;
      }
      locked = true;
      return true;
    }

    void unlock() {
      ++unlock_calls;
      locked = false;
    }

    void set_available(const bool available) {
      available_ = available;
    }

    int blocking_lock_calls {0};
    int try_lock_calls {0};
    int unlock_calls {0};
    bool locked {false};

  private:
    bool available_;
  };
}  // namespace

TEST(DeferredStreamStartPolicy, ContendedLifecycleGatePreservesPendingWorkWithoutBlocking) {
  lifecycle_mutex_stub_t lifecycle_gate {false};
  bool pending = true;
  int apply_calls = 0;

  const auto apply_pending = [&] {
    ++apply_calls;
    pending = false;
    return true;
  };
  const bool initially_applied = stream::deferred_start::try_apply_with_lifecycle_gate(
    lifecycle_gate,
    apply_pending
  );

  EXPECT_FALSE(initially_applied);
  EXPECT_TRUE(pending);
  EXPECT_EQ(apply_calls, 0);
  EXPECT_EQ(lifecycle_gate.blocking_lock_calls, 0);
  EXPECT_EQ(lifecycle_gate.try_lock_calls, 1);
  EXPECT_EQ(lifecycle_gate.unlock_calls, 0);

  lifecycle_gate.set_available(true);
  const bool retried = stream::deferred_start::try_apply_with_lifecycle_gate(
    lifecycle_gate,
    apply_pending
  );

  EXPECT_TRUE(retried);
  EXPECT_FALSE(pending);
  EXPECT_EQ(apply_calls, 1);
  EXPECT_EQ(lifecycle_gate.blocking_lock_calls, 0);
  EXPECT_EQ(lifecycle_gate.try_lock_calls, 2);
  EXPECT_EQ(lifecycle_gate.unlock_calls, 1);
}

TEST(DeferredStreamStartPolicy, ReadyWorkRunsWhileHoldingTheLifecycleGate) {
  lifecycle_mutex_stub_t lifecycle_gate {true};
  bool pending = true;
  bool applied_while_locked = false;

  const bool applied = stream::deferred_start::try_apply_with_lifecycle_gate(
    lifecycle_gate,
    [&] {
      applied_while_locked = lifecycle_gate.locked;
      pending = false;
      return true;
    }
  );

  EXPECT_TRUE(applied);
  EXPECT_TRUE(applied_while_locked);
  EXPECT_FALSE(pending);
  EXPECT_EQ(lifecycle_gate.blocking_lock_calls, 0);
  EXPECT_EQ(lifecycle_gate.try_lock_calls, 1);
  EXPECT_EQ(lifecycle_gate.unlock_calls, 1);
  EXPECT_FALSE(lifecycle_gate.locked);
}

TEST(VideoSendBatchTests, EncryptedDefaultPacketsStayWithinWindowsBufferingLimit) {
  // packetSize=1392 plus the 16-byte RTP allowance, then a 32-byte GCM prefix.
  // The old calculation selected 46 packets: 66,240 bytes instead of <=65,536.
  EXPECT_EQ(stream::video_send_batch_size(1408, 0, 65536), 46u);
  const auto encrypted_count = stream::video_send_batch_size(1408, 32, 65536);
  EXPECT_EQ(encrypted_count, 45u);
  EXPECT_LE(encrypted_count * (1408 + 32), 65536u);
}

TEST(VideoSendBatchTests, HonorsConfiguredByteBudgetAndSegmentationLimit) {
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 16 * 1024), 11u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 128 * 1024), 45u);
  EXPECT_EQ(stream::video_send_batch_size(64, 32, 65536), 64u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 0), 1u);
  EXPECT_EQ(stream::video_send_batch_size(1408, 32, 1000), 1u);
}

TEST(VideoSendBatchTests, EncryptionPrefixNeverPushesABatchPastTheByteLimit) {
  for (std::size_t block_size = 64; block_size <= 9000; ++block_size) {
    for (const std::size_t prefix_size : {0u, 32u}) {
      for (const std::size_t budget : {16u * 1024, 64u * 1024}) {
        const auto count = stream::video_send_batch_size(block_size, prefix_size, budget);
        ASSERT_LE(count * (block_size + prefix_size), budget);
        ASSERT_GE(count, 1u);
        ASSERT_LE(count, 64u);
      }
    }
  }
}

TEST(VideoFormatNameTests, CanonicalCodecNameNormalizesKnownAliases) {
  EXPECT_EQ(stream::canonical_codec_name("h264"), "H.264");
  EXPECT_EQ(stream::canonical_codec_name("H.264"), "H.264");
  EXPECT_EQ(stream::canonical_codec_name("hevc"), "HEVC");
  EXPECT_EQ(stream::canonical_codec_name("H265"), "HEVC");
  EXPECT_EQ(stream::canonical_codec_name("av1"), "AV1");
}

TEST(VideoFormatNameTests, CanonicalCodecNamePreservesUnknownValues) {
  EXPECT_EQ(stream::canonical_codec_name("vp9"), "vp9");
  EXPECT_TRUE(stream::canonical_codec_name({}).empty());
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketParsing, RejectsRuntPacketsBeforeReadingType) {
  EXPECT_FALSE(stream::decode_control_packet({}));

  const char one_byte[] = {'\x34'};
  EXPECT_FALSE(stream::decode_control_packet(std::string_view {one_byte, sizeof(one_byte)}));
}

TEST(ControlPacketParsing, DecodesTypeAndPayloadSafely) {
  const char packet[] = {'\x34', '\x12', 'a', 'b'};

  const auto decoded = stream::decode_control_packet(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_EQ(decoded->payload, "ab");
}

TEST(ControlPacketParsing, AllowsTypeOnlyPacketWithoutPayloadUnderflow) {
  const char packet[] = {'\x34', '\x12'};

  const auto decoded = stream::decode_control_packet(std::string_view {packet, sizeof(packet)});

  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->type, 0x1234);
  EXPECT_TRUE(decoded->payload.empty());
}

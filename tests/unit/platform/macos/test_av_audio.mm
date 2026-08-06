/**
 * @file tests/unit/platform/macos/test_av_audio.mm
 * @brief Platform-audio-independent AV buffer policy tests.
 */
#ifdef __APPLE__

  #include "../../../tests_common.h"

  #include <src/platform/macos/av_audio_policy.h>

  #include <array>
  #include <numeric>

using namespace platf::av_audio_policy;

class AVAudioBufferTest: public testing::TestWithParam<std::uint32_t> {};

INSTANTIATE_TEST_SUITE_P(
  ChannelLayouts,
  AVAudioBufferTest,
  testing::Values(1u, 2u, 8u, 32u)
);

TEST_P(AVAudioBufferTest, InitializeSizesBufferForChannelCount) {
  buffer_t buffer;
  buffer.initialize(GetParam());

  EXPECT_TRUE(buffer.initialized());
  EXPECT_EQ(buffer.capacity(), ring_buffer_bytes(GetParam()));
  EXPECT_EQ(buffer.available(), 0u);
}

TEST(AVAudioBufferPolicy, LifecycleSupportsReinitializeAndRepeatedCleanup) {
  buffer_t buffer;
  EXPECT_FALSE(buffer.initialized());

  buffer.initialize(1);
  const auto mono_capacity = buffer.capacity();
  buffer.initialize(2);
  EXPECT_TRUE(buffer.initialized());
  EXPECT_EQ(buffer.capacity(), mono_capacity * 2);

  buffer.cleanup();
  EXPECT_FALSE(buffer.initialized());
  EXPECT_EQ(buffer.capacity(), 0u);
  EXPECT_EQ(buffer.available(), 0u);
  buffer.cleanup();
  EXPECT_FALSE(buffer.initialized());
}

TEST(AVAudioBufferPolicy, AppendsDeterministicSamplesAndCapsAtCapacity) {
  buffer_t buffer;
  buffer.initialize(1);
  std::vector<float> samples(buffer.capacity() / sizeof(float) + 16);
  std::iota(samples.begin(), samples.end(), 0.0f);

  buffer.append(samples);

  EXPECT_EQ(buffer.available(), buffer.capacity());
  ASSERT_FALSE(buffer.samples().empty());
  EXPECT_FLOAT_EQ(buffer.samples().front(), 0.0f);
  EXPECT_FLOAT_EQ(buffer.samples().back(), static_cast<float>(buffer.samples().size() - 1));
}

TEST(AVAudioBufferPolicy, AppendBeforeInitializationIsIgnored) {
  buffer_t buffer;
  const std::array<float, 2> samples {1.0f, 2.0f};
  buffer.append(samples);
  EXPECT_EQ(buffer.available(), 0u);
}

TEST(AVAudioConverterPolicy, ReturnsRequestedFramesAndAdvancesInput) {
  std::vector<float> samples(256 * 2);
  std::iota(samples.begin(), samples.end(), 0.0f);
  converter_input_t input {samples.data(), 256, 0, 2};

  const auto first = next_converter_chunk(input, 128);
  EXPECT_EQ(first.data, samples.data());
  EXPECT_EQ(first.frames, 128u);
  EXPECT_EQ(first.channels, 2u);
  EXPECT_EQ(first.bytes(), 128u * 2u * sizeof(float));
  EXPECT_EQ(input.provided_frames, 128u);

  const auto second = next_converter_chunk(input, 256);
  EXPECT_EQ(second.data, samples.data() + 256);
  EXPECT_EQ(second.frames, 128u);
  EXPECT_EQ(input.provided_frames, 256u);
}

TEST(AVAudioConverterPolicy, EmptyInvalidAndExhaustedInputReturnNoFrames) {
  converter_input_t empty {};
  EXPECT_EQ(next_converter_chunk(empty, 128).frames, 0u);

  std::array<float, 4> samples {};
  converter_input_t no_channels {samples.data(), 2, 0, 0};
  EXPECT_EQ(next_converter_chunk(no_channels, 2).frames, 0u);

  converter_input_t exhausted {samples.data(), 2, 2, 2};
  EXPECT_EQ(next_converter_chunk(exhausted, 2).frames, 0u);
}

#endif  // __APPLE__

/**
 * @file test_video_timestamp_policy.cpp
 * @brief Source cadence, legacy normalization, and synthetic-frame regressions.
 */
#include "src/video_timestamp_policy.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {
  using policy_t = video::encode_timestamp_policy_t;
  using mode_e = video::source_timestamp_policy_e;
  const auto epoch = policy_t::clock_t::time_point {100s};

  auto at(std::chrono::nanoseconds offset) {
    return epoch + offset;
  }

  std::uint32_t rtp(policy_t::clock_t::time_point stamp) {
    using tick_t = std::chrono::duration<std::uint32_t, std::ratio<1, 90000>>;
    return std::chrono::round<tick_t>(stamp - epoch).count();
  }
}  // namespace

TEST(VideoTimestampPolicy, SteadySourceBelowStreamCeilingRetainsItsSpacing) {
  policy_t source {mode_e::preserve, std::chrono::nanoseconds {1s} / 116};
  for (int i = 0; i < 100; ++i) {
    const auto input = at(i * 10ms);
    const auto output = source.apply(input);
    ASSERT_EQ(output, input);
    EXPECT_EQ(rtp(*output), static_cast<std::uint32_t>(i * 900));
  }
}

TEST(VideoTimestampPolicy, LegacySourceStillNormalizesAndReanchors) {
  policy_t legacy {mode_e::normalize, std::chrono::nanoseconds {1s} / 116};
  const std::array expected {0ns, 8620689ns, 20ms + 0ns, 28620689ns, 40ms + 0ns};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(legacy.apply(at(i * 10ms)), at(expected[i]));
  }
}

TEST(VideoTimestampPolicy, LegacyQuarterIntervalBoundaryIsStrict) {
  policy_t legacy {mode_e::normalize, 10ms};
  EXPECT_EQ(legacy.apply(at(0ms)), at(0ms));
  EXPECT_EQ(legacy.apply(at(12500us)), at(12500us));
  EXPECT_EQ(legacy.apply(at(22400us)), at(22500us));
}

TEST(VideoTimestampPolicy, LegacyEarlyFramesRetainExistingNormalization) {
  policy_t legacy {mode_e::normalize, 10ms};
  EXPECT_EQ(legacy.apply(at(0ms)), at(0ms));
  EXPECT_EQ(legacy.apply(at(8ms)), at(10ms));
  EXPECT_EQ(legacy.apply(at(18ms)), at(20ms));
}

TEST(VideoTimestampPolicy, MissingSyntheticTimestampsDoNotAdvanceRealCadence) {
  for (const auto mode : {mode_e::normalize, mode_e::preserve}) {
    policy_t policy {mode, 10ms};
    EXPECT_EQ(policy.apply(std::nullopt), std::nullopt);
    EXPECT_EQ(policy.apply(at(0ms)), at(0ms));
    EXPECT_EQ(policy.apply(std::nullopt), std::nullopt);
    EXPECT_EQ(policy.apply(std::nullopt), std::nullopt);
    EXPECT_EQ(policy.apply(at(11ms)), at(mode == mode_e::preserve ? 11ms : 10ms));
  }
}

TEST(VideoTimestampPolicy, SourceGapsAndResumeAreNotRetimed) {
  policy_t source {mode_e::preserve, 10ms};
  for (const auto offset : {0ms, 20ms, 24ms, 40ms, 2000ms, 2004ms}) {
    EXPECT_EQ(source.apply(at(offset)), at(offset));
  }
}

TEST(VideoTimestampPolicy, NewDisplayGenerationDoesNotReuseLegacyPhase) {
  policy_t old_generation {mode_e::normalize, 10ms};
  EXPECT_EQ(old_generation.apply(at(0ms)), at(0ms));
  policy_t new_generation {mode_e::normalize, 10ms};
  EXPECT_EQ(new_generation.apply(at(11ms)), at(11ms));
  policy_t presentation_generation {mode_e::preserve, 10ms};
  EXPECT_EQ(presentation_generation.apply(at(11ms)), at(11ms));
}

TEST(VideoTimestampPolicy, FractionalSourceCadenceSurvivesRtpQuantization) {
  policy_t source {mode_e::preserve, std::chrono::nanoseconds {1001s} / 120000};
  std::optional<policy_t::clock_t::time_point> previous;
  for (std::int64_t i = 0; i < 1000; ++i) {
    const auto input = at(std::chrono::nanoseconds {i * 1001000000000LL / 120000});
    const auto output = source.apply(input);
    ASSERT_EQ(output, input);
    if (previous) {
      const auto source_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(*output - *previous).count();
      const auto ticks = static_cast<std::uint32_t>(rtp(*output) - rtp(*previous));
      // The difference between two rounded timestamps is less than one tick.
      const auto error = static_cast<std::int64_t>(ticks) * 1000000000LL - source_ns * 90000;
      EXPECT_GT(error, -1000000000LL);
      EXPECT_LT(error, 1000000000LL);
    }
    previous = output;
  }
}

TEST(VideoTimestampPolicy, RetainedTraceCadenceIsPreservedWithoutLegacySnapping) {
  policy_t source {mode_e::preserve, 8620689ns};
  policy_t legacy {mode_e::normalize, 8620689ns};
  // Retained DRM sequences 223716 through 223718, relative to the first frame.
  const std::array input {0ns, 10775856ns, 22096660ns};
  const std::array legacy_output {0ns, 8620689ns, 22096660ns};
  for (std::size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ(source.apply(at(input[i])), at(input[i]));
    EXPECT_EQ(legacy.apply(at(input[i])), at(legacy_output[i]));
  }
}

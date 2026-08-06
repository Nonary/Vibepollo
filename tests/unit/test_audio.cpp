/**
 * @file tests/unit/test_audio.cpp
 * @brief Deterministic tests for audio stream, sink, and capture policy.
 */
#include "../tests_common.h"

#include <src/audio_policy.h>

#include <deque>

using namespace audio::policy;

TEST(AudioStreamPolicy, SelectsChannelAndQualityVariant) {
  EXPECT_EQ(stream_index(2, false), 0);
  EXPECT_EQ(stream_index(2, true), 1);
  EXPECT_EQ(stream_index(6, false), 2);
  EXPECT_EQ(stream_index(6, true), 3);
  EXPECT_EQ(stream_index(8, false), 4);
  EXPECT_EQ(stream_index(8, true), 5);
  EXPECT_EQ(stream_index(3, true), 0);
}

TEST(AudioStreamPolicy, AppliesCustomSurroundLayoutWithoutMutatingDefault) {
  const stream_layout_t base {6, 4, 2, {0, 1, 4, 5, 2, 3, 0, 0}};
  const stream_layout_t custom {6, 6, 0, {0, 1, 4, 5, 2, 3, 0, 0}};

  EXPECT_EQ(apply_custom_layout(base, std::nullopt).streams, 4);
  const auto selected = apply_custom_layout(base, custom);
  EXPECT_EQ(selected.streams, 6);
  EXPECT_EQ(selected.coupled_streams, 0);
  EXPECT_EQ(base.streams, 4);
}

TEST(AudioSinkPolicy, PreservesPriorityAndEmptyFallbacks) {
  const sink_catalog_t sinks {
    "host",
    "virtual-stereo",
    "virtual-51",
    "virtual-71"
  };

  EXPECT_EQ(select_sink(sinks, "configured", 2, true), "configured");
  EXPECT_EQ(select_sink(sinks, "configured", 6, false), "virtual-51");
  EXPECT_EQ(select_sink(sinks, "", 8, true), "host");

  const sink_catalog_t no_virtual {"", std::nullopt, std::nullopt, std::nullopt};
  EXPECT_TRUE(select_sink(no_virtual, "", 2, false).empty());
}

namespace {
  class fake_source_t: public sample_source_t {
  public:
    std::deque<sample_status_e> statuses;
    std::deque<bool> reacquire_results;

    sample_status_e sample() override {
      if (statuses.empty()) {
        return sample_status_e::interrupted;
      }
      const auto status = statuses.front();
      statuses.pop_front();
      return status;
    }

    bool reacquire() override {
      if (reacquire_results.empty()) {
        return false;
      }
      const bool result = reacquire_results.front();
      reacquire_results.pop_front();
      return result;
    }
  };
}  // namespace

TEST(AudioCapturePolicy, RetainsSuccessTimeoutReinitializeAndStopLifecycle) {
  fake_source_t source;
  source.statuses = {
    sample_status_e::ok,
    sample_status_e::timeout,
    sample_status_e::reinitialize,
    sample_status_e::ok,
    sample_status_e::interrupted,
  };
  source.reacquire_results = {true};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 2u);
  EXPECT_EQ(summary.timeouts, 1u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}

TEST(AudioCapturePolicy, FailedReinitializeStopsWithoutEmitting) {
  fake_source_t source;
  source.statuses = {sample_status_e::reinitialize, sample_status_e::ok};
  source.reacquire_results = {false};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 0u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}

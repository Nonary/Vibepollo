/**
 * @file tests/unit/test_audio.cpp
 * @brief Deterministic tests for audio stream, sink, and capture policy.
 */
#include "../tests_common.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <src/audio_lifecycle_policy.h>
#include <src/audio_lifecycle_state.h>
#include <src/audio_policy.h>
#include <thread>

using namespace audio::policy;

namespace {
  struct lifecycle_token_t {
    int id {};
  };
}  // namespace

TEST(AudioLifecycleState, RetainsAndReclaimsTheSameOwner) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {7};
  lifecycle_token_t reclaimed {};

  ASSERT_TRUE(state.retain(retained, true));
  ASSERT_TRUE(state.reclaim(reclaimed, true));
  EXPECT_EQ(reclaimed.id, 7);
  EXPECT_FALSE(state.terminal_pending());
}

TEST(AudioLifecycleState, BlocksReconnectUntilTerminalRestoreCompletes) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {11};
  ASSERT_TRUE(state.retain(retained, true));
  ASSERT_TRUE(state.begin_terminal());

  std::atomic_bool reconnect_finished {false};
  std::atomic_bool reclaimed {false};
  auto reconnect_waiting = std::promise<void> {};
  auto reconnect_waiting_future = reconnect_waiting.get_future();
  std::thread reconnect([&]() {
    lifecycle_token_t reconnect_token {};
    reclaimed.store(state.reclaim(reconnect_token, true, [&reconnect_waiting]() {
      reconnect_waiting.set_value();
    }),
                    std::memory_order_release);
    reconnect_finished.store(true, std::memory_order_release);
  });

  EXPECT_EQ(
    reconnect_waiting_future.wait_for(std::chrono::seconds {5}),
    std::future_status::ready
  );
  EXPECT_FALSE(reconnect_finished.load(std::memory_order_acquire));

  state.complete_terminal_restore();
  reconnect.join();
  EXPECT_TRUE(reconnect_finished.load(std::memory_order_acquire));
  EXPECT_FALSE(reclaimed.load(std::memory_order_acquire));
}

TEST(AudioLifecycleState, RepeatedTerminalReleaseRestoresAtMostOnce) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  lifecycle_token_t retained {13};
  ASSERT_TRUE(state.retain(retained, true));

  auto first = state.begin_terminal();
  ASSERT_TRUE(first);
  EXPECT_FALSE(state.begin_terminal());
  state.complete_terminal_restore();
  EXPECT_FALSE(state.begin_terminal());
}

TEST(AudioLifecycleState, ContinuousHandoffHasNoIntermediateRestore) {
  audio::lifecycle::state_t<lifecycle_token_t> state;
  state.owner_acquired();
  ASSERT_EQ(state.owner_count(), 1u);

  // The successor app is published while A still owns the shared context.
  ASSERT_FALSE(state.begin_terminal());
  state.app_started();
  state.owner_acquired();
  EXPECT_FALSE(state.terminal_pending());
  EXPECT_EQ(state.owner_count(), 2u);

  state.owner_released();
  EXPECT_EQ(state.owner_count(), 1u);
}

TEST(AudioLifecyclePolicy, RetainsFinalOwnerForResumableApp) {
  using audio::lifecycle::release_action_e;
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, true, false),
    release_action_e::retain
  );
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, true, true),
    release_action_e::restore
  );
  EXPECT_TRUE(audio::lifecycle::can_reclaim_retained_audio(true, true, false));
  EXPECT_FALSE(audio::lifecycle::can_reclaim_retained_audio(true, false, false));
}

TEST(AudioLifecyclePolicy, RestoresTerminalAndIgnoresUnchangedSink) {
  using audio::lifecycle::release_action_e;
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(true, false, false),
    release_action_e::restore
  );
  EXPECT_EQ(
    audio::lifecycle::final_owner_release_action(false, true, false),
    release_action_e::ignore
  );
}

TEST(AudioStreamPolicy, SelectsChannelAndQualityVariant) {
  EXPECT_EQ(stream_index(2, false), 0);
  EXPECT_EQ(stream_index(2, true), 1);
  EXPECT_EQ(stream_index(6, false), 2);
  EXPECT_EQ(stream_index(6, true), 3);
  EXPECT_EQ(stream_index(8, false), 4);
  EXPECT_EQ(stream_index(8, true), 5);
  EXPECT_EQ(stream_index(3, true), 0);
}

TEST(AudioPcmPayload, WritesLittleEndianInt16InInterleavedOrder) {
  // 4660 = 0x1234 and -4660 = 0xEDCC, as exact fractions of 32768
  const std::vector<float> samples {4660.0f / 32768, -4660.0f / 32768, 0.0f};

  EXPECT_EQ(pcm_payload(samples), (std::vector<std::uint8_t> {0x34, 0x12, 0xCC, 0xED, 0x00, 0x00}));
}

TEST(AudioPcmPayload, RoundsToNearestWithHalvesAwayFromZero) {
  // 1.4, 1.5, -1.5, -0.4 and 0.5 steps of 1/32768
  const std::vector<float> samples {1.4f / 32768, 1.5f / 32768, -1.5f / 32768, -0.4f / 32768, 0.5f / 32768};

  // 1, 2, -2, 0, 1
  EXPECT_EQ(pcm_payload(samples), (std::vector<std::uint8_t> {0x01, 0x00, 0x02, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x01, 0x00}));
}

TEST(AudioPcmPayload, ClampsFullScaleAndBeyondToInt16Range) {
  const std::vector<float> samples {1.0f, -1.0f, 1.5f, -1.5f, 1e10f, -1e10f, 32767.0f / 32768, -32767.0f / 32768};

  // 32767, -32768, 32767, -32768, 32767, -32768, 32767, -32767
  EXPECT_EQ(pcm_payload(samples), (std::vector<std::uint8_t> {0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x80}));
}

TEST(AudioPcmPayload, FiveMillisecondsFillStereo51And71Packets) {
  // 5 ms at 48 kHz is 240 samples per channel
  EXPECT_EQ(pcm_payload(std::vector<float>(240 * 2)).size(), 960u);
  EXPECT_EQ(pcm_payload(std::vector<float>(240 * 6)).size(), 2880u);
  EXPECT_EQ(pcm_payload(std::vector<float>(240 * 8)).size(), 3840u);
}

TEST(AudioPcmDatagramBudget, LargestFittingPacketDurationFollowsChannelLayout) {
  for (const bool encrypted : {false, true}) {
    EXPECT_EQ(pcm_max_packet_duration(2, encrypted), 5);
    EXPECT_EQ(pcm_max_packet_duration(4, encrypted), 3);
    EXPECT_EQ(pcm_max_packet_duration(6, encrypted), 2);
    EXPECT_EQ(pcm_max_packet_duration(8, encrypted), 1);
  }
}

TEST(AudioPcmDatagramBudget, LargestFittingDurationKeepsDataAndParityWithinBudget) {
  struct expected_t {
    int channels;
    std::size_t data, parity, encrypted_data, encrypted_parity;
  };

  // Payload is 96 B per channel per ms; data adds a 12 B RTP header, parity also a 12 B FEC header,
  // and encryption pads the payload to the next whole 16 B block (a full block when already aligned).
  for (const auto &test : {
         expected_t {2, 972, 984, 988, 1000},  // 5 ms, 960 B payload
         expected_t {4, 1164, 1176, 1180, 1192},  // 3 ms, 1152 B payload
         expected_t {6, 1164, 1176, 1180, 1192},  // 2 ms, 1152 B payload
         expected_t {8, 780, 792, 796, 808},  // 1 ms, 768 B payload
       }) {
    const auto plain = pcm_datagrams(test.channels, pcm_max_packet_duration(test.channels, false), false);
    const auto encrypted = pcm_datagrams(test.channels, pcm_max_packet_duration(test.channels, true), true);

    EXPECT_EQ(plain.data, test.data) << test.channels;
    EXPECT_EQ(plain.parity, test.parity) << test.channels;
    EXPECT_EQ(encrypted.data, test.encrypted_data) << test.channels;
    EXPECT_EQ(encrypted.parity, test.encrypted_parity) << test.channels;
    for (const auto &datagrams : {plain, encrypted}) {
      EXPECT_LE(datagrams.data, 1400u) << test.channels;
      EXPECT_LE(datagrams.parity, 1400u) << test.channels;
    }
  }
}

TEST(AudioPcmDatagramBudget, NextMillisecondUpDoesNotFit) {
  for (const bool encrypted : {false, true}) {
    EXPECT_TRUE(pcm_packet_duration_fits(2, 5, encrypted));
    EXPECT_FALSE(pcm_packet_duration_fits(2, 6, encrypted)) << "stereo is capped at 5 ms";
    EXPECT_TRUE(pcm_packet_duration_fits(4, 3, encrypted));
    EXPECT_FALSE(pcm_packet_duration_fits(4, 4, encrypted));
    EXPECT_TRUE(pcm_packet_duration_fits(6, 2, encrypted));
    EXPECT_FALSE(pcm_packet_duration_fits(6, 3, encrypted));
    EXPECT_TRUE(pcm_packet_duration_fits(8, 1, encrypted));
    EXPECT_FALSE(pcm_packet_duration_fits(8, 2, encrypted));
  }
}

TEST(AudioPcmDatagramBudget, FiveMillisecondsDoesNotFitSurround) {
  for (const bool encrypted : {false, true}) {
    EXPECT_FALSE(pcm_packet_duration_fits(6, 5, encrypted));
    EXPECT_FALSE(pcm_packet_duration_fits(8, 5, encrypted));
  }
}

TEST(AudioPcmDatagramBudget, DurationsOutsideOneToFiveMillisecondsNeverFit) {
  EXPECT_FALSE(pcm_packet_duration_fits(2, 0, false));
  EXPECT_FALSE(pcm_packet_duration_fits(2, -1, false));
  EXPECT_TRUE(pcm_packet_duration_fits(2, 1, false));
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

TEST(AudioSinkPolicy, ManagedVirtualSinkOverridesHostAudioRequest) {
  const sink_catalog_t sinks {
    "host",
    "sink-sunshine-stereo",
    "sink-sunshine-surround51",
    "sink-sunshine-surround71"
  };

  EXPECT_EQ(
    select_stream_sink(
      sinks,
      "host",
      "sink-sunshine-stereo",
      2,
      true
    ),
    "sink-sunshine-stereo"
  );
  EXPECT_EQ(
    select_stream_sink(
      sinks,
      "host",
      "sink-sunshine-stereo",
      6,
      true
    ),
    "sink-sunshine-surround51"
  );
}

TEST(AudioSinkPolicy, CaptureOnlyRequiresExplicitSelectedSinkAndPreservesVirtualRouting) {
  const sink_catalog_t sinks {"host", "virtual-stereo", "virtual-51", "virtual-71"};
  const auto physical = select_sink(sinks, "second-device", 2, true);
  EXPECT_TRUE(capture_sink_without_routing(true, "second-device", "", physical));
  EXPECT_FALSE(capture_sink_without_routing(false, "second-device", "", physical));
  EXPECT_FALSE(capture_sink_without_routing(true, "", "", "host"));

  // Virtual sinks still take precedence when host audio is disabled or a
  // managed virtual sink is explicitly configured.
  const auto automatic_virtual = select_sink(sinks, "second-device", 2, false);
  EXPECT_FALSE(capture_sink_without_routing(true, "second-device", "", automatic_virtual));
  const auto managed_virtual = select_sink(sinks, "second-device", 2, false);
  EXPECT_FALSE(capture_sink_without_routing(true, "second-device", "second-device", managed_virtual));

  const sink_catalog_t no_virtual {"host", std::nullopt, std::nullopt, std::nullopt};
  const auto selected = select_sink(no_virtual, "second-device", 2, false);
  EXPECT_TRUE(capture_sink_without_routing(true, "second-device", "", selected));
  EXPECT_TRUE(capture_sink_without_routing(true, "host", "", "host"));
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

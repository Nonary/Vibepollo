#include "../tests_common.h"

#include "src/video_policy.h"
#include "src/thread_safe.h"

#include <array>
#include <chrono>
#include <map>

namespace {
  // The parts of platf::converted_frame_t the encode loops' policy reads.
  struct converted_t {
    bool repeats_previous = false;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp;
  };

  class FakeEncoderProvider: public video::policy::encoder_capability_provider_t {
  public:
    std::map<std::string, video::policy::encoder_capabilities_t> values;
    video::policy::encoder_capabilities_t capabilities(std::string_view encoder) const override {
      const auto found = values.find(std::string(encoder));
      return found == values.end() ? video::policy::encoder_capabilities_t {} : found->second;
    }
  };
}

TEST(CapturePolicy, ExactAndSyntheticSourcesRejectProcessDisplayOverride) {
  EXPECT_TRUE(video::policy::may_apply_process_display_preference(video::policy::capture_selection_e::process_preferred));
  EXPECT_FALSE(video::policy::may_apply_process_display_preference(video::policy::capture_selection_e::exact_output));
  EXPECT_FALSE(video::policy::may_apply_process_display_preference(video::policy::capture_selection_e::synthetic_black));
}

TEST(CapturePolicy, ExactOutputRejectsManualSwitchAndActiveOutputKeepsStableIdentity) {
  const std::array<std::string, 2> original_order {"Display-A", "Display-B"};
  const std::array<std::string, 2> reordered {"Display-B", "Display-A"};

  EXPECT_FALSE(video::policy::select_manual_display_output(
    video::policy::capture_selection_e::exact_output,
    1,
    original_order
  ));

  const auto selected = video::policy::select_manual_display_output(
    video::policy::capture_selection_e::process_preferred,
    1,
    original_order
  );
  ASSERT_EQ(selected, "Display-B");
  EXPECT_EQ(video::policy::resolve_display_output(*selected, reordered), 0);
  EXPECT_FALSE(video::policy::resolve_display_output(*selected, std::array<std::string, 1> {"Display-A"}));
}

TEST(CapturePolicy, QueueOverflowRejectsAndSignalsWithoutDiscardingPriorSession) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue {1};
  safe::signal_t shutdown_signal;
  safe::signal_t join_signal;
  ASSERT_TRUE(queue.try_raise(7));

  EXPECT_FALSE(video::policy::try_admit_capture_session(
    queue,
    9,
    shutdown_signal,
    join_signal
  ));
  EXPECT_TRUE(shutdown_signal.peek());
  EXPECT_TRUE(join_signal.peek());
  EXPECT_EQ(queue.pop(0ms), 7);
  EXPECT_FALSE(queue.pop(0ms));
}

TEST(CapturePolicy, AcceptedSessionAdmissionLeavesSignalsForTheWorker) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue {1};
  safe::signal_t shutdown_signal;
  safe::signal_t join_signal;

  EXPECT_TRUE(video::policy::try_admit_capture_session(
    queue,
    7,
    shutdown_signal,
    join_signal
  ));
  EXPECT_FALSE(shutdown_signal.peek());
  EXPECT_FALSE(join_signal.peek());
  EXPECT_EQ(queue.pop(0ms), 7);
}

TEST(CapturePolicy, PersistentDisplayFailureBacksOffWithoutDelayingInitialRecovery) {
  using namespace std::chrono_literals;

  EXPECT_EQ(video::policy::display_retry_delay(0), 50ms);
  EXPECT_EQ(video::policy::display_retry_delay(1), 100ms);
  EXPECT_EQ(video::policy::display_retry_delay(5), 1600ms);
  EXPECT_EQ(video::policy::display_retry_delay(6), 2s);
  EXPECT_EQ(video::policy::display_retry_delay(1000), 2s);

  EXPECT_TRUE(video::policy::should_log_display_retry(0));
  EXPECT_TRUE(video::policy::should_log_display_retry(1));
  EXPECT_TRUE(video::policy::should_log_display_retry(2));
  EXPECT_FALSE(video::policy::should_log_display_retry(3));
  EXPECT_TRUE(video::policy::should_log_display_retry(16));
  EXPECT_FALSE(video::policy::should_log_display_retry(17));
}

TEST(EncoderPolicy, SelectsFirstAvailableCapableEncoderWithoutHardwareProbe) {
  FakeEncoderProvider provider;
  provider.values["nvenc"] = {false, true, true};
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 2> preference {"nvenc", "software"};
  EXPECT_EQ(video::policy::select_encoder(preference, {.hdr = true}, provider), "software");
}

TEST(EncoderPolicy, RejectsEncoderThatCannotMeetRequestedFormat) {
  FakeEncoderProvider provider;
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 1> preference {"software"};
  EXPECT_FALSE(video::policy::select_encoder(preference, {.hdr = true, .yuv444 = true}, provider));
}

struct FramerateX100Test: testing::TestWithParam<std::tuple<std::int32_t, video::policy::rational_t>> {};
TEST_P(FramerateX100Test, Run) {
  const auto &[value, expected] = GetParam();
  EXPECT_EQ(video::policy::framerate_x100_to_rational(value), expected);
}
INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2398, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2500, video::policy::rational_t {25, 1}),
    std::make_tuple(2997, video::policy::rational_t {30000, 1001}),
    std::make_tuple(6000, video::policy::rational_t {60, 1}),
    std::make_tuple(11988, video::policy::rational_t {120000, 1001}),
    std::make_tuple(23976, video::policy::rational_t {240000, 1001}),
    std::make_tuple(9498, video::policy::rational_t {4749, 50})
  )
);

TEST(VideoOutputPolicy, KeepsConfiguredVirtualOutputWhenAnotherVirtualDisplayEnumeratesFirst) {
  const std::array<std::string, 2> active_outputs {
    "\\\\.\\DISPLAY54",
    "\\\\.\\DISPLAY53",
  };

  EXPECT_EQ(
    video::policy::select_preferred_virtual_output(
      "\\\\.\\display53",
      active_outputs,
      active_outputs
    ),
    "\\\\.\\DISPLAY53"
  );
}

TEST(VideoOutputPolicy, FallsBackToFirstActiveVirtualOutputWithoutConfiguredAffinity) {
  const std::array<std::string, 2> active_outputs {
    "\\\\.\\DISPLAY54",
    "\\\\.\\DISPLAY53",
  };

  EXPECT_EQ(
    video::policy::select_preferred_virtual_output("", active_outputs, active_outputs),
    "\\\\.\\DISPLAY54"
  );
}

TEST(ConvertedInputPolicy, NewerFrameReadReplacesTheClaimStamps) {
  const auto claim = std::chrono::steady_clock::time_point {std::chrono::milliseconds {100}};
  const auto frame = std::chrono::steady_clock::time_point {std::chrono::milliseconds {108}};
  const auto host = std::chrono::steady_clock::time_point {std::chrono::milliseconds {110}};
  const auto input = video::policy::resolve_converted_input(converted_t {false, frame, host}, claim, claim);
  EXPECT_TRUE(input.encode);
  EXPECT_EQ(input.frame_timestamp, frame);
  EXPECT_EQ(input.host_processing_timestamp, host);
}

TEST(ConvertedInputPolicy, SameFrameKeepsTheClaimStamps) {
  const auto frame_claim = std::chrono::steady_clock::time_point {std::chrono::milliseconds {100}};
  const auto host_claim = std::chrono::steady_clock::time_point {std::chrono::milliseconds {102}};
  const auto input = video::policy::resolve_converted_input(converted_t {}, frame_claim, host_claim);
  EXPECT_TRUE(input.encode);
  EXPECT_EQ(input.frame_timestamp, frame_claim);
  EXPECT_EQ(input.host_processing_timestamp, host_claim);

  // Placeholders and other images without claim stamps stay unstamped.
  const auto unstamped = video::policy::resolve_converted_input(converted_t {}, std::nullopt, std::nullopt);
  EXPECT_TRUE(unstamped.encode);
  EXPECT_FALSE(unstamped.frame_timestamp);
  EXPECT_FALSE(unstamped.host_processing_timestamp);
}

// A direct image whose frame record could not be read is reported as
// repeats_previous, so it takes the same skip-with-no-encode path.
TEST(ConvertedInputPolicy, RepeatedFrameIsSkippedWithoutStamps) {
  const auto claim = std::chrono::steady_clock::time_point {std::chrono::milliseconds {100}};
  const auto input = video::policy::resolve_converted_input(converted_t {true, claim, claim}, claim, claim);
  EXPECT_FALSE(input.encode);
  EXPECT_FALSE(input.frame_timestamp);
  EXPECT_FALSE(input.host_processing_timestamp);
}

// A skipped repeat must not delay a requested keyframe behind the next capture.
TEST(ConvertedInputPolicy, RepeatedFrameKeepsARequestedKeyframePending) {
  const auto claim = std::chrono::steady_clock::time_point {std::chrono::milliseconds {100}};
  const auto repeated = video::policy::resolve_converted_input(converted_t {true, claim, claim}, claim, claim);
  EXPECT_TRUE(video::policy::retry_idr_after_repeat(repeated, true));
  // A repeat without a keyframe request is a plain skip.
  EXPECT_FALSE(video::policy::retry_idr_after_repeat(repeated, false));
  // A converted frame carries its own keyframe request; there is nothing to retry.
  const auto converted = video::policy::resolve_converted_input(converted_t {}, claim, claim);
  EXPECT_FALSE(video::policy::retry_idr_after_repeat(converted, true));
  EXPECT_FALSE(video::policy::retry_idr_after_repeat(converted, false));
}

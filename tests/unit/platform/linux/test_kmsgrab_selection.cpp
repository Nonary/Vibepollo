/**
 * @file tests/unit/platform/linux/test_kmsgrab_selection.cpp
 * @brief Tests for stable Linux KMS output naming and selection.
 */
#include "../../../tests_common.h"

#include <cstddef>
#include <limits>
#include <src/platform/linux/kmsgrab_pacing.h>
#include <src/platform/linux/kmsgrab_selection.h>
#include <vibeshine_drm_uapi.h>

namespace selection = platf::kms::selection;

TEST(KmsgrabSelection, RejectsInconsistentPresentationResponses) {
  using platf::kms::pacing::classify_response;
  using response_e = platf::kms::pacing::presentation_response_e;

  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 12), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::timeout);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT | VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::timeout);

  EXPECT_EQ(classify_response(0, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
  EXPECT_EQ(classify_response(1U << 31, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 4), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
}

TEST(KmsgrabSelection, BoundsPresentationIoctlErrorHandling) {
  using error_e = platf::kms::pacing::presentation_ioctl_error_e;
  using platf::kms::pacing::classify_ioctl_error;

  EXPECT_EQ(classify_ioctl_error(EINTR, true), error_e::retry);
  EXPECT_EQ(classify_ioctl_error(EAGAIN, true), error_e::retry);
  EXPECT_EQ(classify_ioctl_error(EBUSY, true), error_e::transient_timeout);
  EXPECT_EQ(classify_ioctl_error(ENOTTY, true), error_e::unsupported);

  EXPECT_EQ(classify_ioctl_error(EINTR, false), error_e::unsupported);
  EXPECT_EQ(classify_ioctl_error(EAGAIN, false), error_e::unsupported);
  EXPECT_EQ(classify_ioctl_error(EBUSY, false), error_e::unsupported);
}

TEST(KmsgrabSelection, RetainsCaptureAcrossPendingCommitTransitions) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  latch.observe_response(response_e::changed, true, start);
  EXPECT_TRUE(latch.state_pending());
  EXPECT_FALSE(latch.capture_ready());
  EXPECT_FALSE(latch.pending_timed_out(start + 99ms, 100ms));
  EXPECT_TRUE(latch.pending_timed_out(start + 100ms, 100ms));

  latch.observe_response(response_e::timeout, false, start + 100ms);
  EXPECT_FALSE(latch.state_pending());
  EXPECT_TRUE(latch.capture_ready());

  latch.mark_delivered(latch.capture_generation());
  EXPECT_FALSE(latch.capture_ready());
}

TEST(KmsgrabSelection, RepeatedPendingChangesKeepTheirOriginalFallbackDeadline) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  for (int elapsed_ms = 0; elapsed_ms <= 96; elapsed_ms += 16) {
    latch.observe_response(response_e::changed, true, start + std::chrono::milliseconds {elapsed_ms});
  }
  EXPECT_FALSE(latch.pending_timed_out(start + 99ms, 100ms));
  EXPECT_TRUE(latch.pending_timed_out(start + 100ms, 100ms));

  EXPECT_TRUE(platf::kms::pacing::hold_pending_response_to_deadline(response_e::changed, true, true));
  EXPECT_FALSE(platf::kms::pacing::hold_pending_response_to_deadline(response_e::changed, true, false));
  EXPECT_FALSE(platf::kms::pacing::hold_pending_response_to_deadline(response_e::changed, false, true));
  EXPECT_FALSE(platf::kms::pacing::hold_pending_response_to_deadline(response_e::timeout, true, true));
}

TEST(KmsgrabSelection, PendingPresentationUsesAHangDeadlineInsteadOfTheOldFallbackWindow) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};
  latch.observe_response(response_e::changed, true, start);

  EXPECT_FALSE(latch.pending_timed_out(start + 500ms, platf::kms::pacing::PRESENT_PENDING_HANG_TIMEOUT));
  EXPECT_TRUE(latch.pending_timed_out(start + 5s, platf::kms::pacing::PRESENT_PENDING_HANG_TIMEOUT));
}

TEST(KmsgrabSelection, RequiredPresentationModeNeverEnablesFixedRateFallback) {
  platf::kms::pacing::presentation_mode_t required_mode {true};
  EXPECT_FALSE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  required_mode.activate();
  EXPECT_TRUE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  required_mode.deactivate();
  EXPECT_FALSE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  platf::kms::pacing::presentation_mode_t ordinary_kms;
  EXPECT_FALSE(ordinary_kms.event_capture_enabled());
  EXPECT_TRUE(ordinary_kms.fixed_rate_allowed());
}

TEST(KmsgrabSelection, EventDeliveryTracksSameRatePresentationsAndCoalescesOversupply) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  EXPECT_EQ(limiter.next_delivery(first), first);

  limiter.mark_delivered(first);
  EXPECT_EQ(limiter.next_delivery(first + 1ms), first + 10ms);
  EXPECT_EQ(limiter.next_delivery(first + 9ms), first + 10ms);

  // A near-deadline source timestamp moves the cadence with the compositor.
  const auto same_rate_presentation = first + 9500us;
  limiter.mark_delivered(same_rate_presentation);
  EXPECT_EQ(limiter.next_delivery(first + 10ms), first + 19500us);

  // A materially early presentation retains the client-rate deadline.
  const auto oversupplied_presentation = first + 15ms;
  limiter.mark_delivered(oversupplied_presentation);
  EXPECT_EQ(limiter.next_delivery(first + 20ms), first + 29500us);

  // A late source frame rebases instead of producing a catch-up burst.
  const auto stalled_delivery = first + 45ms;
  limiter.mark_delivered(stalled_delivery);
  EXPECT_EQ(limiter.next_delivery(stalled_delivery), stalled_delivery + 10ms);

  limiter.reset();
  EXPECT_EQ(limiter.next_delivery(stalled_delivery + 1ms), stalled_delivery + 1ms);
}

TEST(KmsgrabSelection, OlderSnapshotCannotClearNewerPresentation) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  latch.request_capture();
  const auto snapshot_generation = latch.capture_generation();

  latch.observe_response(response_e::changed, false, start);
  latch.mark_delivered(snapshot_generation);
  EXPECT_TRUE(latch.capture_ready());

  latch.observe_response(response_e::changed, true, start + 1ms);
  EXPECT_FALSE(latch.capture_ready());
  latch.observe_response(response_e::timeout, false, start + 2ms);
  EXPECT_TRUE(latch.capture_ready());

  latch.mark_delivered(latch.capture_generation());
  EXPECT_FALSE(latch.capture_ready());
}

TEST(KmsgrabSelection, RejectsUnsafePresentationTimestamps) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  const auto now = clock_t::time_point {10s};
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(0, now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::numeric_limits<std::uint64_t>::max(), now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(12s).count(), now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(10001ms).count(), now, 0ns), std::nullopt);

  const auto valid_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(9500ms).count();
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 1s), clock_t::time_point {9500ms});
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 0ns, clock_t::time_point {9600ms}), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 0ns, clock_t::time_point {9500ms}), clock_t::time_point {9500ms});
}

TEST(KmsgrabSelection, UsesStableVibeshinePresentationAbi) {
  EXPECT_EQ(VIBESHINE_DRM_PRESENT_ABI_VERSION, 1u);
  EXPECT_EQ(VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS, 1000u);
  EXPECT_EQ(DRM_VIBESHINE_WAIT_PRESENT, 0u);
  EXPECT_EQ(sizeof(vibeshine_drm_wait_present), 48u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, sequence), 8u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, timestamp_ns), 16u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, timeout_ms), 24u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, reserved), 32u);

  EXPECT_EQ(VIBESHINE_DRM_FRAME_ABI_VERSION, 1u);
  EXPECT_EQ(VIBESHINE_DRM_FRAME_MAX_PLANES, 4u);
  EXPECT_EQ(DRM_VIBESHINE_GET_FRAME, 1u);
  EXPECT_EQ(sizeof(vibeshine_drm_frame), 152u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, sequence), 8u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, modifier), 40u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, dma_buf_fds), 52u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, sync_file_fds), 100u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, reserved), 120u);
}

TEST(KmsgrabSelection, RecognizesCudaImportableDisplayDrivers) {
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm-extra"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("vibeshine_drm"));

  EXPECT_FALSE(selection::driver_supports_cuda_import("vkms"));
  EXPECT_FALSE(selection::driver_supports_cuda_import("i915"));
  EXPECT_FALSE(selection::driver_supports_cuda_import(""));
  EXPECT_FALSE(selection::driver_is_nvidia("vibeshine_drm"));
  EXPECT_TRUE(selection::driver_requires_direct_import("vibeshine_drm"));
  EXPECT_TRUE(selection::driver_requires_presentation_events("vibeshine_drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("nvidia-drm"));
  EXPECT_FALSE(selection::driver_requires_presentation_events("nvidia-drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("vkms"));
  EXPECT_FALSE(selection::driver_requires_presentation_events("vkms"));
}

TEST(KmsgrabSelection, RejectsMissingOrAmbiguousDriverIdentities) {
  EXPECT_FALSE(selection::normalize_driver_name(nullptr, 0));
  EXPECT_FALSE(selection::normalize_driver_name("", 0));

  const char embedded_nul[] {'v', 'k', 'm', 's', '\0', 'x'};
  EXPECT_FALSE(selection::normalize_driver_name(embedded_nul, sizeof(embedded_nul)));

  const std::string oversized(selection::MAX_DRM_DRIVER_NAME_LENGTH + 1, 'x');
  EXPECT_FALSE(selection::normalize_driver_name(oversized.data(), oversized.size()));

  EXPECT_EQ(selection::normalize_driver_name("vibeshine_drm", 13), "vibeshine_drm");
}

TEST(KmsgrabSelection, ParsesOnlyCompleteUnsignedNumericAliases) {
  EXPECT_EQ(selection::parse_numeric_alias("0"), 0u);
  EXPECT_EQ(selection::parse_numeric_alias("17"), 17u);
  EXPECT_EQ(selection::parse_numeric_alias(std::to_string(std::numeric_limits<std::uint32_t>::max())), std::numeric_limits<std::uint32_t>::max());

  EXPECT_FALSE(selection::parse_numeric_alias(""));
  EXPECT_FALSE(selection::parse_numeric_alias("-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("1-HDMI-A-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("4294967296"));
}

TEST(KmsgrabSelection, UsesStableConnectorNamesAndLegacyMonitorOrdinals) {
  const auto monitors = selection::name_monitors({
    {"card1", "HDMI-A-1", 52, 1},
    {"card2", "Virtual-1", 40, 0},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "Virtual-1");
  EXPECT_EQ(monitors[1].display_name, "HDMI-A-1");

  const auto stable_match = selection::resolve_named_monitor("virtual-1", monitors);
  ASSERT_TRUE(stable_match);
  EXPECT_EQ(stable_match->card_path, "card2");
  EXPECT_EQ(stable_match->crtc_id, 40u);
  EXPECT_EQ(stable_match->monitor_index, 0u);

  const auto numeric_alias = selection::parse_numeric_alias("1");
  ASSERT_TRUE(numeric_alias);
  EXPECT_EQ(*numeric_alias, monitors[1].monitor.monitor_index);
}

TEST(KmsgrabSelection, QualifiesCrossGpuCollisionsAndRejectsAmbiguousRawName) {
  const auto monitors = selection::name_monitors({
    {"card0", "HDMI-A-1", 10, 0},
    {"card1", "HDMI-A-1", 20, 1},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "card0:HDMI-A-1");
  EXPECT_EQ(monitors[1].display_name, "card1:HDMI-A-1");
  EXPECT_FALSE(selection::resolve_named_monitor("HDMI-A-1", monitors));

  const auto match = selection::resolve_named_monitor("CARD1:hdmi-a-1", monitors);
  ASSERT_TRUE(match);
  EXPECT_EQ(match->card_path, "card1");
  EXPECT_EQ(match->crtc_id, 20u);
}

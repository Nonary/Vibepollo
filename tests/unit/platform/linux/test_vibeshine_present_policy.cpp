/**
 * @file tests/unit/platform/linux/test_vibeshine_present_policy.cpp
 * @brief Executable tests for the policy helpers used by vibeshine_drm.
 */
#include <gtest/gtest.h>

#include <vibeshine_drm_present.h>
#include <vibeshine_drm_change.h>
#include <vibeshine_drm_vrr.h>

TEST(VibeshinePresentPolicy, PreservesFlipTimeAcrossDelayedWorkerAndLaterVblanks) {
  vibeshine_drm_present_timing timing {};
  vibeshine_drm_present_timing_arm(&timing);
  constexpr __u64 flip_ns = 1'000'000'000;
  EXPECT_TRUE(vibeshine_drm_present_timing_complete(&timing, true, flip_ns));
  EXPECT_FALSE(vibeshine_drm_present_timing_complete(&timing, true, flip_ns + 8'333'333));
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&timing, flip_ns + 65'000'000), flip_ns);
}

TEST(VibeshinePresentPolicy, IgnoresVblanksBeforeTheExactFlipCompletes) {
  vibeshine_drm_present_timing timing {};
  vibeshine_drm_present_timing_arm(&timing);
  EXPECT_FALSE(vibeshine_drm_present_timing_complete(&timing, false, 100));
  EXPECT_FALSE(vibeshine_drm_present_timing_complete(&timing, true, 0));
  EXPECT_EQ(timing.timestamp_ns, 0u);
  EXPECT_TRUE(timing.pending);
  EXPECT_TRUE(vibeshine_drm_present_timing_complete(&timing, true, 200));
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&timing, 300), 200u);
}

TEST(VibeshinePresentPolicy, DetachesTimedOutFlipBeforeLateCompletion) {
  vibeshine_drm_present_timing timing {};
  vibeshine_drm_present_timing_arm(&timing);
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&timing, 300), 300u);
  EXPECT_FALSE(timing.pending);
  EXPECT_FALSE(vibeshine_drm_present_timing_complete(&timing, true, 400));
  EXPECT_EQ(timing.timestamp_ns, 0u);
}

TEST(VibeshinePresentPolicy, ImmediateCommitUsesCompletionTimeNotWorkerTime) {
  vibeshine_drm_present_timing timing {};
  vibeshine_drm_present_timing_arm(&timing);
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&timing, true, 100));
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&timing, 300), 100u);
}

TEST(VibeshinePresentPolicy, ResetsTimingForEachCommitAndKeepsCrtcsIndependent) {
  vibeshine_drm_present_timing first {};
  vibeshine_drm_present_timing second {};
  vibeshine_drm_present_timing_arm(&first);
  vibeshine_drm_present_timing_arm(&second);
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&first, true, 100));
  EXPECT_EQ(second.timestamp_ns, 0u);
  EXPECT_TRUE(second.pending);
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&second, true, 200));
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&first, 300), 100u);
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&second, 300), 200u);
  vibeshine_drm_present_timing_arm(&first);
  EXPECT_EQ(first.timestamp_ns, 0u);
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&first, true, 400));
  EXPECT_EQ(vibeshine_drm_present_timing_finish(&first, 500), 400u);
}

TEST(VibeshinePresentPolicy, VariableWorkerDelayDoesNotChangeSourceFrameSpacing) {
  vibeshine_drm_present_timing first {};
  vibeshine_drm_present_timing second {};
  constexpr __u64 first_flip_ns = 1'000'000'000;
  constexpr __u64 period_ns = 8'333'333;
  vibeshine_drm_present_timing_arm(&first);
  vibeshine_drm_present_timing_arm(&second);
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&first, true, first_flip_ns));
  ASSERT_TRUE(vibeshine_drm_present_timing_complete(&second, true, first_flip_ns + period_ns));
  const auto first_source = vibeshine_drm_present_timing_finish(&first, first_flip_ns + 1'000'000);
  const auto second_source = vibeshine_drm_present_timing_finish(&second, first_flip_ns + period_ns + 20'000'000);
  EXPECT_EQ(second_source - first_source, period_ns);
}

TEST(VibeshinePresentPolicy, ValidatesEveryRequestField) {
  vibeshine_drm_wait_present request {};
  request.abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION;
  request.timeout_ms = VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS;
  EXPECT_TRUE(vibeshine_drm_present_request_valid(&request));

  request.abi_version++;
  EXPECT_FALSE(vibeshine_drm_present_request_valid(&request));
  request.abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION;

  request.timeout_ms++;
  EXPECT_FALSE(vibeshine_drm_present_request_valid(&request));
  request.timeout_ms = 0;

  request.reserved[0] = 1;
  EXPECT_FALSE(vibeshine_drm_present_request_valid(&request));
  request.reserved[0] = 0;
  request.reserved[1] = 1;
  EXPECT_FALSE(vibeshine_drm_present_request_valid(&request));
}

TEST(VibeshinePresentPolicy, DecidesQueriesWaitsAndCapacityBoundaries) {
  vibeshine_drm_wait_present request {};
  request.abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION;
  request.sequence = 7;
  request.timeout_ms = 16;

  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, VIBESHINE_DRM_PRESENT_MAX_WAITERS - 1),
    VIBESHINE_DRM_PRESENT_REGISTER_WAITER
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_REJECT_BUSY
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 8, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_RETURN_CURRENT
  );

  request.timeout_ms = 0;
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_RETURN_CURRENT
  );
}

TEST(VibeshinePresentPolicy, ConstructsChangedPendingAndTimeoutResponses) {
  vibeshine_drm_wait_present request {};

  vibeshine_drm_present_complete_response(&request, 7, 9, 1234, true);
  EXPECT_EQ(request.sequence, 9u);
  EXPECT_EQ(request.timestamp_ns, 1234u);
  EXPECT_EQ(request.flags, VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_PENDING);

  vibeshine_drm_present_complete_response(&request, 9, 9, 5678, false);
  EXPECT_EQ(request.sequence, 9u);
  EXPECT_EQ(request.timestamp_ns, 5678u);
  EXPECT_EQ(request.flags, VIBESHINE_DRM_PRESENT_TIMEOUT);
}

TEST(VibeshinePresentPolicy, SuppressesOnlyProvablePlaneNoOps) {
  vibeshine_drm_plane_snapshot old_state {};
  old_state.present = true;
  old_state.visible = true;
  old_state.crtc = 1;
  old_state.framebuffer = 2;
  old_state.crtc_w = 3840;
  old_state.crtc_h = 2160;
  old_state.src_w = 3840U << 16;
  old_state.src_h = 2160U << 16;
  old_state.alpha = 0xffff;
  auto new_state = old_state;

  EXPECT_FALSE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));

  new_state.framebuffer = 3;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
  new_state = old_state;

  new_state.content_update = true;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
  new_state = old_state;

  new_state.crtc_x = 1;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
  new_state = old_state;

  new_state.color_range = 1;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
}

TEST(VibeshinePresentPolicy, DetectsCrtcAndConnectorOutputChanges) {
  vibeshine_drm_crtc_snapshot old_crtc {};
  old_crtc.present = true;
  old_crtc.enable = true;
  old_crtc.active = true;
  old_crtc.mode_blob = 10;
  auto new_crtc = old_crtc;
  EXPECT_FALSE(vibeshine_drm_crtc_changes_scanout(&old_crtc, &new_crtc));

  new_crtc.gamma_lut = 11;
  EXPECT_TRUE(vibeshine_drm_crtc_changes_scanout(&old_crtc, &new_crtc));
  new_crtc = old_crtc;
  new_crtc.color_mgmt_changed = true;
  EXPECT_TRUE(vibeshine_drm_crtc_changes_scanout(&old_crtc, &new_crtc));

  vibeshine_drm_connector_snapshot old_connector {};
  old_connector.present = true;
  old_connector.crtc = 1;
  old_connector.hdr_output_metadata = 20;
  auto new_connector = old_connector;
  EXPECT_FALSE(vibeshine_drm_connector_changes_scanout(&old_connector, &new_connector));

  new_connector.hdr_output_metadata = 21;
  EXPECT_TRUE(vibeshine_drm_connector_changes_scanout(&old_connector, &new_connector));
  new_connector = old_connector;
  new_connector.crtc = 2;
  EXPECT_TRUE(vibeshine_drm_connector_changes_scanout(&old_connector, &new_connector));
}

TEST(VibeshinePresentPolicy, BoundsVrrPresentationsByTheModePeriod) {
  constexpr unsigned long long period_ns = 8'333'333;

  EXPECT_EQ(vibeshine_drm_vrr_presentation_deadline_ns(0, 100, period_ns), 100);
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(1'000'000'000, 1'007'000'000, period_ns),
    1'008'333'333
  );
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(1'000'000'000, 1'010'000'000, period_ns),
    1'010'000'000
  );
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(~0ULL - 2, 100, 10),
    ~0ULL
  );
}

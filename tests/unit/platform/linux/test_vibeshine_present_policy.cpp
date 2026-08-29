/**
 * @file tests/unit/platform/linux/test_vibeshine_present_policy.cpp
 * @brief Executable tests for the policy helpers used by vibeshine_drm.
 */
#include <gtest/gtest.h>

#include <vibeshine_drm_present.h>
#include <vibeshine_drm_change.h>

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

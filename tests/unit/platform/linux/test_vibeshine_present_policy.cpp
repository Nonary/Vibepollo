/**
 * @file tests/unit/platform/linux/test_vibeshine_present_policy.cpp
 * @brief Executable tests for the policy helpers used by vibeshine_drm.
 */
#include <gtest/gtest.h>

#include <vibeshine_drm_present.h>

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
    vibeshine_drm_present_decide_wait(&request, 7, false, VIBESHINE_DRM_PRESENT_MAX_WAITERS - 1),
    VIBESHINE_DRM_PRESENT_REGISTER_WAITER
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, false, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_REJECT_BUSY
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 8, false, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_RETURN_CURRENT
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 8, true, 0),
    VIBESHINE_DRM_PRESENT_REGISTER_WAITER
  );

  request.timeout_ms = 0;
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, true, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
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

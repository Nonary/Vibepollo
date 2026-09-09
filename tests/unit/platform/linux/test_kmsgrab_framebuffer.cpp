/**
 * @file tests/unit/platform/linux/test_kmsgrab_framebuffer.cpp
 * @brief GEM framebuffer ownership checks without a DRM device.
 */
#include "../../../tests_common.h"

#include <algorithm>
#include <src/platform/linux/kmsgrab_framebuffer.h>
#include <vector>

namespace {
  TEST(KmsFramebuffer, ClosesDistinctHandlesAndSkipsUnusedPlanes) {
    std::uint32_t handles[] {17, 0, 42, 91};
    std::vector<std::uint32_t> closed;

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      closed.push_back(handle);
    });

    EXPECT_EQ(closed, (std::vector<std::uint32_t> {17, 42, 91}));
    EXPECT_TRUE(std::ranges::all_of(handles, [](auto handle) {
      return handle == 0;
    }));

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      ADD_FAILURE() << "Closed a handle after ownership was released: " << handle;
    });
  }

  TEST(KmsFramebuffer, SharedPlanesDoNotCloseAReusedHandleNumber) {
    std::uint32_t handles[] {17, 42, 17, 42};
    std::vector<std::uint32_t> closed;
    bool unrelated_handle_17_exists = false;

    platf::kms::close_framebuffer_handles(handles, [&](std::uint32_t handle) {
      // Another caller can allocate the same numeric handle after GEM_CLOSE.
      // A second close for a plane alias would release that unrelated object.
      if (handle == 17) {
        EXPECT_FALSE(unrelated_handle_17_exists);
        unrelated_handle_17_exists = true;
      }
      closed.push_back(handle);
    });

    EXPECT_EQ(closed, (std::vector<std::uint32_t> {17, 42}));
    EXPECT_TRUE(unrelated_handle_17_exists);
    EXPECT_TRUE(std::ranges::all_of(handles, [](auto handle) {
      return handle == 0;
    }));
  }
}  // namespace

/** @file tests/unit/platform/linux/test_pyrowave_linear_upload.cpp
 * @brief Behavioral tests for PyroWave's bounded linear DMA-BUF fallback.
 */
#include "../../../tests_common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <src/platform/linux/pyrowave_linear_upload.h>
#include <unistd.h>

namespace {
  using pyrowave::linux_gpu::linear_upload::backing_extent_matches;
  using pyrowave::linux_gpu::linear_upload::can_upload_surface;
  using pyrowave::linux_gpu::linear_upload::copy_mapped_rows;
  using pyrowave::linux_gpu::linear_upload::requires_external_image_barriers;
  using pyrowave::linux_gpu::linear_upload::retry_sync_call;
  using pyrowave::linux_gpu::linear_upload::should_attempt_linear_upload;

  TEST(PyrowaveLinearUpload, CopiesActiveBytesFromPitchedSurfaceWithOffset) {
    egl::surface_descriptor_t surface;
    surface.width = 2;
    surface.height = 2;
    surface.fds[0] = 3;
    surface.fourcc = DRM_FORMAT_XRGB8888;
    surface.modifier = DRM_FORMAT_MOD_LINEAR;
    surface.pitches[0] = 12;
    surface.offsets[0] = 4;
    surface.direct_import_required = true;

    std::vector<std::uint8_t> mapped(24, 0xee);
    const std::array<std::uint8_t, 8> row0 {1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::uint8_t, 8> row1 {9, 10, 11, 12, 13, 14, 15, 16};
    std::copy(row0.begin(), row0.end(), mapped.begin() + 4);
    std::copy(row1.begin(), row1.end(), mapped.begin() + 16);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(can_upload_surface(surface));
    EXPECT_TRUE(should_attempt_linear_upload(surface, true));
    EXPECT_FALSE(should_attempt_linear_upload(surface, false));
    ASSERT_TRUE(copy_mapped_rows(surface, mapped, pixels));
    EXPECT_EQ(pixels, (std::vector<std::uint8_t> {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}));
  }

  TEST(PyrowaveLinearUpload, RejectsTiledImplicitAndUnknownFormats) {
    egl::surface_descriptor_t surface;
    surface.width = 2;
    surface.height = 2;
    surface.fds[0] = 3;
    surface.fourcc = DRM_FORMAT_XRGB8888;
    surface.pitches[0] = 8;
    surface.modifier = 0x0300000000606013ULL;
    std::vector<std::uint8_t> mapped(16, 0);
    std::vector<std::uint8_t> pixels {0xaa};

    EXPECT_FALSE(can_upload_surface(surface));
    EXPECT_FALSE(should_attempt_linear_upload(surface, true));
    EXPECT_FALSE(copy_mapped_rows(surface, mapped, pixels));
    EXPECT_EQ(pixels, (std::vector<std::uint8_t> {0xaa}));

    surface.modifier = DRM_FORMAT_MOD_INVALID;
    surface.direct_import_required = false;
    EXPECT_FALSE(can_upload_surface(surface));

    surface.modifier = DRM_FORMAT_MOD_LINEAR;
    surface.fourcc = DRM_FORMAT_NV12;
    EXPECT_FALSE(can_upload_surface(surface));
  }

  TEST(PyrowaveLinearUpload, RejectsShortPitchAndTruncatedMappings) {
    egl::surface_descriptor_t surface;
    surface.width = 2;
    surface.height = 2;
    surface.fds[0] = 3;
    surface.fourcc = DRM_FORMAT_XRGB8888;
    surface.modifier = DRM_FORMAT_MOD_LINEAR;
    surface.pitches[0] = 7;
    std::vector<std::uint8_t> pixels;
    EXPECT_FALSE(can_upload_surface(surface));

    surface.pitches[0] = 8;
    std::vector<std::uint8_t> truncated(15, 0);
    EXPECT_TRUE(can_upload_surface(surface));
    EXPECT_FALSE(copy_mapped_rows(surface, truncated, pixels));
  }

  TEST(PyrowaveLinearUpload, ChecksRequiredExtentAgainstBackingObject) {
    FILE *file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    const int fd = fileno(file);
    ASSERT_EQ(ftruncate(fd, 24), 0);
    EXPECT_TRUE(backing_extent_matches(fd, 24));
    EXPECT_FALSE(backing_extent_matches(fd, 25));
    EXPECT_FALSE(backing_extent_matches(fd, 0));
    EXPECT_FALSE(backing_extent_matches(-1, 1));
    std::fclose(file);
  }

  TEST(PyrowaveLinearUpload, RetriesInterruptedAndTemporarilyUnavailableSyncCalls) {
    int calls = 0;
    const auto result = retry_sync_call([&] {
      ++calls;
      if (calls < 3) {
        errno = calls == 1 ? EINTR : EAGAIN;
        return -1;
      }
      return 0;
    });
    EXPECT_EQ(result, 0);
    EXPECT_EQ(calls, 3);

    calls = 0;
    const auto permanent_error = retry_sync_call([&] {
      ++calls;
      errno = EIO;
      return -1;
    });
    EXPECT_EQ(permanent_error, -1);
    EXPECT_EQ(calls, 1);
  }

  TEST(PyrowaveLinearUpload, UsesForeignBarriersOnlyForExternalImages) {
    EXPECT_TRUE(requires_external_image_barriers(true, false));
    EXPECT_FALSE(requires_external_image_barriers(true, true));
    EXPECT_FALSE(requires_external_image_barriers(false, false));
  }
}  // namespace

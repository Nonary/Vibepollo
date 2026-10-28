/**
 * @file tests/unit/platform/linux/test_dmabuf_surface.cpp
 * @brief Capture failure and descriptor-reuse regressions using real file descriptors.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/dmabuf_surface.h>

#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <sys/eventfd.h>

namespace {
  TEST(DmabufSurface, LayoutDiagnosticIncludesSinglePlaneXrgbImportLayout) {
    egl::surface_descriptor_t surface;
    surface.width = 2190;
    surface.height = 1179;
    surface.fourcc = 0x34325258;  // DRM_FORMAT_XRGB8888 uses one image plane.
    surface.modifier = 0x0100000000000001;
    surface.fds[0] = 17;
    surface.pitches[0] = 8960;
    surface.offsets[0] = 0;

    EXPECT_EQ(egl::describe_surface_layout(surface),
              "width=2190 height=1179 fourcc=0x34325258 modifier=0x100000000000001 "
              "fd_slot_count=1 fd_slots=[0{pitch=8960,offset=0}]");
  }

  TEST(DmabufSurface, LayoutDiagnosticPreservesSlotsOfMalformedSparseDescriptor) {
    egl::surface_descriptor_t surface;
    surface.width = 2190;
    surface.height = 1179;
    surface.fourcc = 0x34325258;  // Deliberately malformed: XRGB has no second image plane.
    surface.modifier = 0x0100000000000001;
    surface.fds[0] = 17;
    surface.pitches[0] = 8960;
    surface.offsets[0] = 0;
    surface.fds[2] = 19;
    surface.pitches[2] = 4480;
    surface.offsets[2] = 10557440;

    EXPECT_EQ(egl::describe_surface_layout(surface),
              "width=2190 height=1179 fourcc=0x34325258 modifier=0x100000000000001 "
              "fd_slot_count=2 fd_slots=[0{pitch=8960,offset=0},2{pitch=4480,offset=10557440}]");
  }

  TEST(DmabufSurface, CpuFallbackRequiresKnownLayoutForManagedImports) {
    egl::surface_descriptor_t surface;
    surface.direct_import_required = true;
    surface.modifier = 0;
    EXPECT_TRUE(egl::can_cpu_upload_surface(surface));

    surface.modifier = 0x00ffffffffffffffULL;
    EXPECT_FALSE(egl::can_cpu_upload_surface(surface));
    surface.direct_import_required = false;
    EXPECT_TRUE(egl::can_cpu_upload_surface(surface));

    // The block-linear modifier observed in the NVIDIA scanout probe.
    surface.modifier = 0x0300000000606013ULL;
    EXPECT_FALSE(egl::can_cpu_upload_surface(surface));
    surface.direct_import_required = true;
    EXPECT_FALSE(egl::can_cpu_upload_surface(surface));
  }

  TEST(DmabufSurface, SuccessfulDirectImportNeverUploads) {
    egl::surface_descriptor_t surface;
    int imports = 0;
    int uploads = 0;
    const auto result = egl::import_source_with_cpu_fallback(surface,
      [&](const auto &) { ++imports; return std::optional<int> {17}; },
      [&](const auto &) { ++uploads; return std::optional<int> {23}; });
    EXPECT_EQ(result, 17);
    EXPECT_EQ(imports, 1);
    EXPECT_EQ(uploads, 0);
  }

  TEST(DmabufSurface, FailedLinearImportUploadsWithoutChangingRequestedLayout) {
    for (const auto &[width, height, pitch] : {
           std::tuple {3440, 1440, 13760u},
           std::tuple {3440, 1440, 13824u},
           std::tuple {3456, 1440, 13824u},
           std::tuple {2190, 1179, 8960u}}) {
      egl::surface_descriptor_t surface;
      surface.width = width;
      surface.height = height;
      surface.pitches[0] = pitch;
      surface.offsets[0] = 4096;
      surface.fds[0] = 17;
      surface.direct_import_required = true;
      surface.modifier = 0;  // Explicitly linear managed framebuffer.
      int imports = 0;
      int uploads = 0;
      const auto result = egl::import_source_with_cpu_fallback(surface,
        [&](const auto &descriptor) {
          ++imports;
          EXPECT_EQ(&descriptor, &surface);
          return std::optional<int> {};
        },
        [&](const auto &descriptor) {
          ++uploads;
          EXPECT_EQ(imports, 1);
          EXPECT_EQ(&descriptor, &surface);
          EXPECT_EQ(descriptor.width, width);
          EXPECT_EQ(descriptor.height, height);
          EXPECT_EQ(descriptor.pitches[0], pitch);
          EXPECT_EQ(descriptor.offsets[0], 4096u);
          EXPECT_EQ(descriptor.fds[0], 17);
          EXPECT_EQ(descriptor.modifier, 0u);
          return std::optional<int> {23};
        });
      EXPECT_EQ(result, 23);
      EXPECT_EQ(imports, 1);
      EXPECT_EQ(uploads, 1);
    }
  }

  TEST(DmabufSurface, FailedUnknownOrTiledManagedImportNeverUploads) {
    for (const auto modifier : {0x00ffffffffffffffULL, 0x0300000000606013ULL}) {
      egl::surface_descriptor_t surface;
      surface.direct_import_required = true;
      surface.modifier = modifier;
      int imports = 0;
      int uploads = 0;
      const auto result = egl::import_source_with_cpu_fallback(surface,
        [&](const auto &) { ++imports; return std::optional<int> {}; },
        [&](const auto &) { ++uploads; return std::optional<int> {23}; });
      EXPECT_FALSE(result);
      EXPECT_EQ(imports, 1);
      EXPECT_EQ(uploads, 0);
    }
  }

  TEST(DmabufSurface, FailedCpuUploadRemainsAnImportFailure) {
    egl::surface_descriptor_t surface;
    int uploads = 0;
    const auto result = egl::import_source_with_cpu_fallback(surface,
      [](const auto &) { return std::optional<int> {}; },
      [&](const auto &) { ++uploads; return std::optional<int> {}; });
    EXPECT_FALSE(result);
    EXPECT_EQ(uploads, 1);
  }

  int open_frame_fd() {
    return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  }

  void expect_closed(int fd) {
    errno = 0;
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
  }

  TEST(DmabufSurface, EmptySurfaceOwnsNoDescriptors) {
    egl::owned_surface_t surface;
    for (int fd : surface.sd.fds) {
      EXPECT_EQ(fd, -1);
    }
    egl::reset_surface(surface.sd);
  }

  TEST(DmabufSurface, FailedFrameCannotCloseReusedDescriptorDuringImageReset) {
    egl::owned_surface_t image;
    int failed_frame_fd;
    {
      egl::owned_surface_t pending;
      pending.sd.fds[0] = open_frame_fd();
      ASSERT_GE(pending.sd.fds[0], 0);
      failed_frame_fd = pending.sd.fds[0];
      // A mode change or a later-plane export failure abandons preparation.
      // Nothing has been transferred into the reusable capture image.
    }
    expect_closed(failed_frame_fd);

    egl::owned_surface_t unrelated;
    unrelated.sd.fds[0] = open_frame_fd();
    ASSERT_EQ(unrelated.sd.fds[0], failed_frame_fd);
    egl::reset_surface(image.sd);
    EXPECT_GE(fcntl(unrelated.sd.fds[0], F_GETFD), 0);
    const std::uint64_t value = 1;
    EXPECT_EQ(write(unrelated.sd.fds[0], &value, sizeof(value)), sizeof(value));
  }

  TEST(DmabufSurface, PartialMultiPlaneFailureClosesEveryAcquiredDescriptor) {
    int first;
    int second;
    {
      egl::owned_surface_t pending;
      first = pending.sd.fds[0] = open_frame_fd();
      second = pending.sd.fds[1] = open_frame_fd();
      ASSERT_GE(first, 0);
      ASSERT_GE(second, 0);
      // Preparing the third plane fails; neither of these descriptors escapes.
    }
    expect_closed(first);
    expect_closed(second);
  }

  TEST(DmabufSurface, SuccessfulTransferClosesExactlyOnce) {
    egl::owned_surface_t image;
    int frame_fd;
    {
      egl::owned_surface_t pending;
      frame_fd = pending.sd.fds[0] = open_frame_fd();
      ASSERT_GE(frame_fd, 0);
      pending.sd.width = 3840;
      pending.sd.height = 2160;
      pending.sd.pitches[0] = 15360;
      image.sd = pending.release();
      EXPECT_EQ(pending.sd.fds[0], -1);
    }
    EXPECT_GE(fcntl(frame_fd, F_GETFD), 0);
    EXPECT_EQ(image.sd.width, 3840);
    EXPECT_EQ(image.sd.height, 2160);
    EXPECT_EQ(image.sd.pitches[0], 15360u);
    egl::reset_surface(image.sd);
    expect_closed(frame_fd);

    egl::owned_surface_t unrelated;
    unrelated.sd.fds[0] = open_frame_fd();
    ASSERT_EQ(unrelated.sd.fds[0], frame_fd);
    egl::reset_surface(image.sd);
    EXPECT_GE(fcntl(unrelated.sd.fds[0], F_GETFD), 0);
  }

  TEST(DmabufSurface, PreparationExceptionClosesOnlyUnpublishedDescriptors) {
    egl::owned_surface_t image;
    int frame_fd = -1;
    EXPECT_THROW({
      egl::owned_surface_t pending;
      frame_fd = pending.sd.fds[0] = open_frame_fd();
      if (frame_fd < 0) {
        throw std::runtime_error("eventfd unavailable");
      }
      throw std::runtime_error("frame validation failed");
    }, std::runtime_error);
    ASSERT_GE(frame_fd, 0);
    expect_closed(frame_fd);
    EXPECT_EQ(image.sd.fds[0], -1);
  }

  TEST(DmabufSurface, ExceptionAfterPublicationLeavesImageAsTheOnlyOwner) {
    egl::owned_surface_t image;
    int frame_fd = -1;
    EXPECT_THROW({
      egl::owned_surface_t pending;
      frame_fd = pending.sd.fds[0] = open_frame_fd();
      if (frame_fd < 0) {
        throw std::runtime_error("eventfd unavailable");
      }
      image.sd = pending.release();
      // Cursor allocation and gamma metadata work occur after publication.
      throw std::runtime_error("later snapshot operation failed");
    }, std::runtime_error);
    ASSERT_GE(frame_fd, 0);
    EXPECT_GE(fcntl(frame_fd, F_GETFD), 0);
    egl::reset_surface(image.sd);
    expect_closed(frame_fd);
  }
}  // namespace

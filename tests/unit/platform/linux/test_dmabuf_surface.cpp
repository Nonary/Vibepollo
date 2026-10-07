/**
 * @file tests/unit/platform/linux/test_dmabuf_surface.cpp
 * @brief Capture failure and descriptor-reuse regressions using real file descriptors.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/dmabuf_surface.h>

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/eventfd.h>

namespace {
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

/**
 * @file src/platform/linux/dmabuf_linear_upload.h
 * @brief Safe CPU-copy policy for explicitly linear capture surfaces.
 */
#pragma once

#include "dmabuf_surface.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <drm_fourcc.h>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vector>

namespace egl::linear_upload {
  inline unsigned pixel_size(std::uint32_t fourcc) noexcept {
    switch (fourcc) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
      case DRM_FORMAT_RGBA8888:
      case DRM_FORMAT_BGRA8888:
      case DRM_FORMAT_XRGB2101010:
      case DRM_FORMAT_ARGB2101010:
      case DRM_FORMAT_XBGR2101010:
      case DRM_FORMAT_ABGR2101010:
        return 4;
      case DRM_FORMAT_XRGB16161616:
      case DRM_FORMAT_ARGB16161616:
      case DRM_FORMAT_XBGR16161616:
      case DRM_FORMAT_ABGR16161616:
      case DRM_FORMAT_XRGB16161616F:
      case DRM_FORMAT_ARGB16161616F:
      case DRM_FORMAT_XBGR16161616F:
      case DRM_FORMAT_ABGR16161616F:
        return 8;
      default:
        return 0;
    }
  }

  inline bool required_mapped_size(const egl::surface_descriptor_t &surface, std::size_t &size) noexcept {
    if (surface.width <= 0 || surface.height <= 0 || !surface.pitches[0]) {
      return false;
    }

    const auto width = static_cast<std::size_t>(surface.width);
    const auto height = static_cast<std::size_t>(surface.height);
    const auto bpp = pixel_size(surface.fourcc);
    if (!bpp || width > static_cast<std::size_t>(std::numeric_limits<int>::max()) / bpp) {
      return false;
    }
    const auto row_bytes = width * bpp;
    const auto pitch = static_cast<std::size_t>(surface.pitches[0]);
    const auto offset = static_cast<std::size_t>(surface.offsets[0]);
    if (pitch < row_bytes || height - 1 > (std::numeric_limits<std::size_t>::max() - offset) / pitch) {
      return false;
    }
    const auto last_row = offset + (height - 1) * pitch;
    if (row_bytes > std::numeric_limits<std::size_t>::max() - last_row) {
      return false;
    }
    size = last_row + row_bytes;
    return true;
  }

  inline bool backing_extent_matches(int fd, std::size_t required_size) noexcept {
    if (fd < 0 || required_size == 0) {
      return false;
    }
    struct stat backing {};
    if (fstat(fd, &backing) != 0 || backing.st_size < 0) {
      return false;
    }
    return static_cast<std::uintmax_t>(backing.st_size) >= required_size;
  }

  template<typename SyncCall>
  inline int retry_sync_call(SyncCall sync_call) {
    int result;
    do {
      result = sync_call();
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result;
  }

  inline bool can_upload_surface(const egl::surface_descriptor_t &surface) noexcept {
    std::size_t ignored = 0;
    return surface.modifier == DRM_FORMAT_MOD_LINEAR && surface.fds[0] >= 0 &&
           surface.fds[1] < 0 && surface.fds[2] < 0 && surface.fds[3] < 0 && required_mapped_size(surface, ignored);
  }

  inline bool should_attempt_linear_upload(const egl::surface_descriptor_t &surface, bool producer_ready) noexcept {
    return producer_ready && can_upload_surface(surface);
  }

  inline bool requires_external_image_barriers(bool imported_surface, bool replaced_with_local_upload) noexcept {
    return imported_surface && !replaced_with_local_upload;
  }

  // Copy active pixels only. This function is shared by the production
  // DMA-BUF mapping path and component tests; row padding is never uploaded.
  inline bool copy_mapped_rows(const egl::surface_descriptor_t &surface, std::span<const std::uint8_t> mapped, std::vector<std::uint8_t> &pixels) {
    std::size_t mapped_size = 0;
    if (!can_upload_surface(surface) || !required_mapped_size(surface, mapped_size) || mapped.size() < mapped_size) {
      return false;
    }

    const auto row_bytes = static_cast<std::size_t>(surface.width) * pixel_size(surface.fourcc);
    const auto pitch = static_cast<std::size_t>(surface.pitches[0]);
    const auto offset = static_cast<std::size_t>(surface.offsets[0]);
    const auto output_size = row_bytes * static_cast<std::size_t>(surface.height);
    pixels.resize(output_size);
    for (int row = 0; row < surface.height; ++row) {
      std::copy_n(mapped.data() + offset + static_cast<std::size_t>(row) * pitch, row_bytes, pixels.data() + static_cast<std::size_t>(row) * row_bytes);
    }
    return true;
  }

  inline bool read_pixels(const surface_descriptor_t &s, std::vector<std::uint8_t> &pixels, std::string &error) {
    if (!can_upload_surface(s)) {
      error = "capture surface is not a supported linear CPU upload";
      return false;
    }
    std::size_t mapped_size = 0;
    if (!required_mapped_size(s, mapped_size)) {
      error = "linear DMA-BUF layout size overflow";
      return false;
    }
    if (!backing_extent_matches(s.fds[0], mapped_size)) {
      error = "linear DMA-BUF backing extent is unavailable or smaller than its layout";
      return false;
    }
    void *mapped = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, s.fds[0], 0);
    if (mapped == MAP_FAILED) {
      error = "mapping linear capture DMA-BUF failed";
      return false;
    }

    struct mapping_guard_t {
      void *address;
      std::size_t size;

      ~mapping_guard_t() {
        munmap(address, size);
      }
    } mapping {mapped, mapped_size};

    dma_buf_sync sync {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    const auto sync_result = retry_sync_call([&] {
      return ioctl(s.fds[0], DMA_BUF_IOCTL_SYNC, &sync);
    });
    if (sync_result < 0) {
      error = "synchronizing linear capture DMA-BUF for CPU read failed";
      return false;
    }

    struct sync_guard_t {
      int fd;
      bool active = true;

      ~sync_guard_t() {
        if (active) {
          dma_buf_sync end {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
          retry_sync_call([&] {
            return ioctl(fd, DMA_BUF_IOCTL_SYNC, &end);
          });
        }
      }

      bool finish() {
        dma_buf_sync end {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
        const auto result = retry_sync_call([&] {
          return ioctl(fd, DMA_BUF_IOCTL_SYNC, &end);
        });
        active = false;
        return result == 0;
      }
    } cpu_access {s.fds[0]};

    if (!copy_mapped_rows(s, std::span(static_cast<const std::uint8_t *>(mapped), mapped_size), pixels)) {
      error = "copying linear capture DMA-BUF pixels failed";
      return false;
    }
    if (!cpu_access.finish()) {
      error = "synchronizing linear capture DMA-BUF after CPU read failed";
      return false;
    }
    return true;
  }
}  // namespace egl::linear_upload

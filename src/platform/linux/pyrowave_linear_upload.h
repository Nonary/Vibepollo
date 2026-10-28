/**
 * @file src/platform/linux/pyrowave_linear_upload.h
 * @brief Safe CPU-copy policy for explicitly linear PyroWave capture surfaces.
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
#include <vector>

namespace pyrowave::linux_gpu::linear_upload {
  inline bool supports_fourcc(std::uint32_t fourcc) noexcept {
    switch (fourcc) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        return true;
      default:
        return false;
    }
  }

  inline bool required_mapped_size(const egl::surface_descriptor_t &surface, std::size_t &size) noexcept {
    if (surface.width <= 0 || surface.height <= 0 || !surface.pitches[0]) {
      return false;
    }

    const auto width = static_cast<std::size_t>(surface.width);
    const auto height = static_cast<std::size_t>(surface.height);
    if (width > static_cast<std::size_t>(std::numeric_limits<int>::max() / 4) || width > std::numeric_limits<std::size_t>::max() / 4) {
      return false;
    }
    const auto row_bytes = width * 4;
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
    return surface.modifier == DRM_FORMAT_MOD_LINEAR &&
           egl::can_cpu_upload_surface(surface) && surface.fds[0] >= 0 &&
           supports_fourcc(surface.fourcc) && required_mapped_size(surface, ignored);
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

    const auto row_bytes = static_cast<std::size_t>(surface.width) * 4;
    const auto pitch = static_cast<std::size_t>(surface.pitches[0]);
    const auto offset = static_cast<std::size_t>(surface.offsets[0]);
    const auto output_size = row_bytes * static_cast<std::size_t>(surface.height);
    pixels.resize(output_size);
    for (int row = 0; row < surface.height; ++row) {
      std::copy_n(mapped.data() + offset + static_cast<std::size_t>(row) * pitch, row_bytes, pixels.data() + static_cast<std::size_t>(row) * row_bytes);
    }
    return true;
  }
}  // namespace pyrowave::linux_gpu::linear_upload

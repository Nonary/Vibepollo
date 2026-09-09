/**
 * @file src/platform/linux/kmsgrab_framebuffer.h
 * @brief Release GEM handles owned by a captured KMS framebuffer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace platf::kms {
  template<typename CloseHandle>
  void close_framebuffer_handles(std::span<std::uint32_t> handles, CloseHandle &&close_handle) {
    // GETFB2 can return the same GEM handle for several planes. Handles are
    // not reference-counted: clear every alias before closing the handle once.
    // https://docs.kernel.org/gpu/drm-uapi.html#drm-ioctl-mode-getfb2
    for (std::size_t plane = 0; plane < handles.size(); ++plane) {
      const auto handle = handles[plane];
      if (!handle) {
        continue;
      }
      for (std::size_t alias = plane; alias < handles.size(); ++alias) {
        if (handles[alias] == handle) {
          handles[alias] = 0;
        }
      }
      close_handle(handle);
    }
  }
}  // namespace platf::kms

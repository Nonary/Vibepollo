/**
 * @file src/platform/linux/dmabuf_surface.h
 * @brief DMA-BUF descriptors and ownership during capture preparation.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <unistd.h>

namespace egl {
  struct surface_descriptor_t {
    int width {};
    int height {};
    int fds[4] {-1, -1, -1, -1};
    std::uint32_t fourcc {};
    std::uint64_t modifier {};
    std::uint32_t pitches[4] {};
    std::uint32_t offsets[4] {};
    bool direct_import_required {};
  };

  inline void reset_surface(surface_descriptor_t &surface) noexcept {
    for (auto &fd : surface.fds) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
  }

  // Keep a partially prepared frame private until capture has validated it.
  // Publishing raw FD numbers while another owner can close them makes a
  // later image reset close unrelated descriptors that reused those numbers.
  class owned_surface_t {
  public:
    owned_surface_t() = default;
    owned_surface_t(const owned_surface_t &) = delete;
    owned_surface_t &operator=(const owned_surface_t &) = delete;

    ~owned_surface_t() {
      reset_surface(sd);
    }

    surface_descriptor_t release() noexcept {
      auto result = sd;
      std::fill_n(sd.fds, 4, -1);
      return result;
    }

    surface_descriptor_t sd;
  };
}  // namespace egl

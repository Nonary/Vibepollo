/**
 * @file src/platform/linux/dmabuf_surface.h
 * @brief DMA-BUF descriptors and ownership during capture preparation.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sstream>
#include <string>
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

  inline std::string describe_surface_layout(const surface_descriptor_t &surface) {
    std::ostringstream description;
    description << "width=" << surface.width
                << " height=" << surface.height
                << " fourcc=0x" << std::hex << surface.fourcc
                << " modifier=0x" << surface.modifier
                << std::dec;

    std::size_t fd_slot_count = 0;
    for (const int fd : surface.fds) {
      fd_slot_count += static_cast<std::size_t>(fd >= 0);
    }

    description << " fd_slot_count=" << fd_slot_count << " fd_slots=[";
    bool first_fd_slot = true;
    for (std::size_t slot = 0; slot < std::size(surface.fds); ++slot) {
      if (surface.fds[slot] < 0) {
        continue;
      }
      if (!first_fd_slot) {
        description << ',';
      }
      first_fd_slot = false;
      description << slot << "{pitch=" << surface.pitches[slot]
                  << ",offset=" << surface.offsets[slot] << '}';
    }
    description << ']';
    return description.str();
  }

  inline bool can_cpu_upload_surface(const surface_descriptor_t &surface) noexcept {
    constexpr std::uint64_t implicit_modifier = 0x00ffffffffffffffULL;
    // Managed imports retain the exporter's layout. An implicit modifier is
    // unknown, so those bytes cannot safely be read as linear pixels.
    return surface.modifier == 0 ||
      (surface.modifier == implicit_modifier && !surface.direct_import_required);
  }

  // Prefer zero-copy import. Only fall back to reading pixels when the
  // descriptor's layout is safe to interpret on the CPU; never rewrite the
  // producer's logical dimensions, pitch, offset, or modifier to force import.
  // Callbacks keep this policy testable without a live EGL/VAAPI device.
  template<class Import, class Upload>
  auto import_source_with_cpu_fallback(const surface_descriptor_t &surface, Import &&import, Upload &&upload) {
    auto result = import(surface);
    if (!result && can_cpu_upload_surface(surface)) {
      result = upload(surface);
    }
    return result;
  }

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

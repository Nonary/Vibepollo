/** @file src/platform/linux/pyrowave_core.h
 * @brief Vulkan PyroWave encoder, independent of capture and network lifetimes.
 */
#pragma once
#include "dmabuf_surface.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pyrowave::linux_gpu {
  struct config_t {
    int width = 0, height = 0;
    bool yuv444 = false, ten_bit = false, hdr = false;
    std::array<float, 12> matrix {};
    std::string render_device;
  };

  struct source_t {
    const egl::surface_descriptor_t *surface = nullptr;
    // RAM capture and test images are BGRA8. Stride is in bytes.
    const std::uint8_t *pixels = nullptr;
    int width = 0, height = 0, stride = 0;
    int offset_x = 0, offset_y = 0;
    bool y_invert = false;
    const std::uint8_t *cursor = nullptr;
    int cursor_width = 0, cursor_height = 0;
    int cursor_x = 0, cursor_y = 0, cursor_dst_width = 0, cursor_dst_height = 0;
    const std::vector<std::array<std::uint16_t, 3>> *lut = nullptr;
  };

  struct packet_t {
    std::size_t offset, size;
  };

  class core_t {
  public:
    static std::unique_ptr<core_t> create(const config_t &config, std::string &error);
    ~core_t();
    bool encode(const source_t &source, std::size_t budget, std::size_t boundary, std::vector<std::uint8_t> &bitstream, std::vector<packet_t> &packets, std::string &error);

  private:
    struct impl_t;
    core_t();
    std::unique_ptr<impl_t> impl;
  };
}  // namespace pyrowave::linux_gpu

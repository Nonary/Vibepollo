/** @file src/platform/linux/pyrowave_capture.h
 * @brief Select DMA-BUF or RAM pixels from a captured Linux frame.
 */
#pragma once

#include "graphics.h"
#include "pyrowave_core.h"

#include <optional>
#include <string>

namespace pyrowave::linux_gpu {
  struct prepared_capture_t {
    source_t source;
    std::string render_device;
  };

  inline std::optional<prepared_capture_t> prepare_capture_source(const platf::img_t &image, int capture_width, int capture_height) {
    prepared_capture_t prepared;
    if (auto *img = dynamic_cast<const egl::img_descriptor_t *>(&image)) {
      if (!img->sequence) {
        return std::nullopt;
      }
      if (img->sd.fds[0] >= 0) {
        prepared.source.surface = &img->sd;
        prepared.source.width = capture_width;
        prepared.source.height = capture_height;
        prepared.source.offset_x = img->capture_offset_x;
        prepared.source.offset_y = img->capture_offset_y;
        prepared.source.y_invert = img->y_invert;
        prepared.source.cursor = img->data;
        prepared.source.cursor_width = img->src_w;
        prepared.source.cursor_height = img->src_h;
        prepared.source.cursor_x = img->x;
        prepared.source.cursor_y = img->y;
        prepared.source.cursor_dst_width = img->width;
        prepared.source.cursor_dst_height = img->height;
        prepared.source.lut = img->crtc_gamma_lut.get();
        prepared.render_device = img->capture_render_device;
        return prepared;
      }
      // PipeWire's RAM frames also inherit img_descriptor_t. KMS uses data for
      // cursor pixels, so only PipeWire metadata permits a RAM fallback here.
      if (!img->pw_flags.has_value()) {
        return std::nullopt;
      }
      prepared.source.y_invert = img->y_invert;
      prepared.source.lut = img->crtc_gamma_lut.get();
    }
    if (!image.data || image.pixel_pitch != 4 || image.width <= 0 || image.height <= 0 ||
        image.row_pitch < std::int64_t(image.width) * 4) {
      return std::nullopt;
    }
    prepared.source.pixels = image.data;
    prepared.source.width = image.width;
    prepared.source.height = image.height;
    prepared.source.stride = image.row_pitch;
    return prepared;
  }
}  // namespace pyrowave::linux_gpu

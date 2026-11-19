/**
 * @file src/platform/linux/dmabuf_egl.h
 * @brief EGL imports shared by desktop GL and the GLES capture fallback.
 */
#pragma once

#include <glad/egl.h>
#include <vector>

#include "dmabuf_surface.h"

namespace egl {
  struct plane_attr_t {
    EGLAttrib fd;
    EGLAttrib offset;
    EGLAttrib pitch;
    EGLAttrib lo;
    EGLAttrib hi;
  };

  inline plane_attr_t get_plane(std::uint32_t plane_indice) {
    switch (plane_indice) {
      case 0:
        return {
          EGL_DMA_BUF_PLANE0_FD_EXT,
          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          EGL_DMA_BUF_PLANE0_PITCH_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        };
      case 1:
        return {
          EGL_DMA_BUF_PLANE1_FD_EXT,
          EGL_DMA_BUF_PLANE1_OFFSET_EXT,
          EGL_DMA_BUF_PLANE1_PITCH_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        };
      case 2:
        return {
          EGL_DMA_BUF_PLANE2_FD_EXT,
          EGL_DMA_BUF_PLANE2_OFFSET_EXT,
          EGL_DMA_BUF_PLANE2_PITCH_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        };
      case 3:
        return {
          EGL_DMA_BUF_PLANE3_FD_EXT,
          EGL_DMA_BUF_PLANE3_OFFSET_EXT,
          EGL_DMA_BUF_PLANE3_PITCH_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
        };
    }

    // Avoid warning
    return {};
  }

  /**
   * @brief Get EGL attributes for eglCreateImage() to import the provided surface.
   * @param surface The surface descriptor.
   * @return Vector of EGL attributes.
   */
  inline std::vector<EGLAttrib> surface_descriptor_to_egl_attribs(const surface_descriptor_t &surface) {
    std::vector<EGLAttrib> attribs;

    attribs.emplace_back(EGL_WIDTH);
    attribs.emplace_back(surface.width);
    attribs.emplace_back(EGL_HEIGHT);
    attribs.emplace_back(surface.height);
    attribs.emplace_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.emplace_back(surface.fourcc);

    for (auto x = 0; x < 4; ++x) {
      auto fd = surface.fds[x];
      if (fd < 0) {
        continue;
      }

      auto plane_attr = get_plane(x);

      attribs.emplace_back(plane_attr.fd);
      attribs.emplace_back(fd);
      attribs.emplace_back(plane_attr.offset);
      attribs.emplace_back(surface.offsets[x]);
      attribs.emplace_back(plane_attr.pitch);
      attribs.emplace_back(surface.pitches[x]);

      if (surface.modifier != IMPLICIT_DMA_BUF_MODIFIER) {
        attribs.emplace_back(plane_attr.lo);
        attribs.emplace_back(surface.modifier & 0xFFFFFFFF);
        attribs.emplace_back(plane_attr.hi);
        attribs.emplace_back(surface.modifier >> 32);
      }
    }

    attribs.emplace_back(EGL_NONE);
    return attribs;
  }

  inline EGLImage create_dmabuf_image(EGLDisplay display, const surface_descriptor_t &surface) {
    const auto attribs = surface_descriptor_to_egl_attribs(surface);
    return eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
  }

}  // namespace egl

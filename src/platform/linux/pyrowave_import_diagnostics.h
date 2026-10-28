/**
 * @file src/platform/linux/pyrowave_import_diagnostics.h
 * @brief Capture modifier checks and bounded rejection details; no import fallback.
 */
#pragma once

#include "dmabuf_surface.h"

#include <drm_fourcc.h>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace pyrowave::linux_gpu::import_diagnostics {
  enum class rejection_e {
    none,
    unsupported_format,
    implicit_modifier,
    invalid_fd,
    modifier_not_advertised,
    invalid_plane_count
  };

  struct check_t {
    rejection_e rejection;
    std::size_t advertised_modifiers = 0;
    bool matched = false;
    std::uint32_t plane_count = 0;
    VkFormatFeatureFlags tiling_features = 0;
  };

  // Keep the importer's existing acceptance rules. Feature bits are evidence,
  // not a new gate; Granite validates the complete image usage at creation.
  inline check_t classify(const egl::surface_descriptor_t &surface, VkFormat format, const std::vector<VkDrmFormatModifierPropertiesEXT> &modifiers) {
    if (format == VK_FORMAT_UNDEFINED) {
      return {rejection_e::unsupported_format};
    }
    if (surface.modifier == DRM_FORMAT_MOD_INVALID) {
      return {rejection_e::implicit_modifier};
    }
    if (surface.fds[0] < 0) {
      return {rejection_e::invalid_fd};
    }
    for (const auto &modifier : modifiers) {
      if (modifier.drmFormatModifier == surface.modifier) {
        return {
          modifier.drmFormatModifierPlaneCount && modifier.drmFormatModifierPlaneCount <= 4 ? rejection_e::none : rejection_e::invalid_plane_count,
          modifiers.size(),
          true,
          modifier.drmFormatModifierPlaneCount,
          modifier.drmFormatModifierTilingFeatures
        };
      }
    }
    return {rejection_e::modifier_not_advertised, modifiers.size()};
  }

  struct device_t {
    // The selection path may name a primary card; render IDs below belong to
    // the Vulkan device that initialized successfully.
    std::string configured_device;
    std::string gpu_name;
    std::uint32_t vendor_id = 0, device_id = 0;
    bool has_render = false;
    std::int64_t render_major = 0, render_minor = 0;
  };

  inline std::string bounded_text(std::string_view text) {
    constexpr std::size_t limit = 96;
    std::string result;
    for (const unsigned char c : text.substr(0, limit)) {
      result += c >= 32 && c <= 126 && c != '"' && c != '\\' ? char(c) : '?';
    }
    if (text.size() > limit) {
      result += "...";
    }
    return result;
  }

  // Only called on rejection. No per-frame success logging or full capability
  // dump: two bounded strings and fixed-width numeric fields keep this <768 B.
  inline std::string describe(const egl::surface_descriptor_t &surface, VkFormat format, const check_t &check, const device_t &device) {
    const char *reason = "unknown-rejection";
    const char *prefix = "capture format or explicit DMA-BUF modifier unsupported";
    switch (check.rejection) {
      case rejection_e::none:
        return {};
      case rejection_e::unsupported_format:
        reason = "unsupported-format";
        break;
      case rejection_e::implicit_modifier:
        reason = "implicit-modifier";
        break;
      case rejection_e::invalid_fd:
        reason = "invalid-fd";
        break;
      case rejection_e::modifier_not_advertised:
        prefix = "GPU cannot import capture modifier";
        reason = "modifier-not-advertised";
        break;
      case rejection_e::invalid_plane_count:
        prefix = "GPU cannot import capture modifier";
        reason = "invalid-plane-count";
        break;
    }
    std::string fourcc;
    for (unsigned i = 0; i < 4; ++i) {
      const auto c = (surface.fourcc >> (i * 8)) & 0xff;
      fourcc += c >= 32 && c <= 126 && c != '"' && c != '\\' ? char(c) : '?';
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << prefix << " (reason=" << reason
        << ", configured_device=\"" << bounded_text(device.configured_device.empty() ? "(automatic)" : device.configured_device)
        << "\", gpu=\"" << bounded_text(device.gpu_name)
        << "\", vendor=0x" << std::hex << device.vendor_id << ", device=0x" << device.device_id
        << ", drm_render=" << std::dec;
    if (device.has_render) {
      out << device.render_major << ':' << device.render_minor;
    } else {
      out << "unavailable";
    }
    out << ", fourcc=\"" << fourcc << "\"/0x" << std::hex << surface.fourcc
        << ", VkFormat=" << std::dec << static_cast<int>(format)
        << ", modifier=0x" << std::hex << surface.modifier
        << ", advertised_modifiers=" << std::dec;
    if (check.rejection == rejection_e::modifier_not_advertised || check.rejection == rejection_e::invalid_plane_count) {
      out << check.advertised_modifiers;
    } else {
      out << "not-queried";
    }
    out << ", matching_planes=";
    if (check.matched) {
      out << check.plane_count << ", matching_tiling_features=0x" << std::hex << check.tiling_features;
    } else {
      out << "unavailable, matching_tiling_features=unavailable";
    }
    out << ')';
    return out.str();
  }
}  // namespace pyrowave::linux_gpu::import_diagnostics

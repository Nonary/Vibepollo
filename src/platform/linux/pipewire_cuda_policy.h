/**
 * @file src/platform/linux/pipewire_cuda_policy.h
 * @brief Pure policy helpers for selecting PipeWire CUDA interop paths.
 */
#pragma once

#include <string_view>

namespace platf::linux_pipewire {
  /**
   * @brief Determine whether the EGL device used for Wayland capture is NVIDIA.
   *
   * The EGL vendor identifies the device that will import the compositor's
   * negotiated DMA-BUFs. The inventory of other DRM devices is not relevant:
   * an inactive Intel GPU may coexist with a KWin session running on NVIDIA.
   */
  [[nodiscard]] constexpr bool egl_vendor_supports_cuda_dmabuf(std::string_view vendor) noexcept {
    return vendor.contains("NVIDIA");
  }
}  // namespace platf::linux_pipewire

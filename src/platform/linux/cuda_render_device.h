/**
 * @file src/platform/linux/cuda_render_device.h
 * @brief Open only the render node belonging to the selected CUDA PCI device.
 */
#pragma once

#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <string_view>

namespace cuda {
  inline int open_render_node_for_pci_device(
    std::string_view pci_bus_id,
    const std::filesystem::path &pci_devices = "/sys/bus/pci/devices",
    const std::filesystem::path &dri = "/dev/dri"
  ) {
    // CUDA returns dddd:bb:dd.f, sometimes with uppercase hexadecimal.
    // Resolve only that PCI device; an unrelated GPU is not a substitute.
    if (pci_bus_id.size() != 12 || pci_bus_id[4] != ':' ||
        pci_bus_id[7] != ':' || pci_bus_id[10] != '.') {
      errno = EINVAL;
      return -1;
    }
    std::string address {pci_bus_id};
    for (std::size_t index = 0; index < address.size(); ++index) {
      if (index == 4 || index == 7 || index == 10) {
        continue;
      }
      auto &digit = address[index];
      if (digit >= 'A' && digit <= 'F') {
        digit += 'a' - 'A';
      }
      if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
        errno = EINVAL;
        return -1;
      }
    }

    std::error_code error;
    std::filesystem::path renderer;
    const auto drm_directory = pci_devices / address / "drm";
    for (std::filesystem::directory_iterator it(drm_directory, error), end;
         !error && it != end; it.increment(error)) {
      const auto name = it->path().filename().string();
      constexpr std::string_view prefix = "renderD";
      if (!name.starts_with(prefix) || name.size() == prefix.size() ||
          name.find_first_not_of("0123456789", prefix.size()) != std::string::npos) {
        continue;
      }
      // DRM advertises one render node per device. Reject ambiguous topology
      // instead of letting directory enumeration order select the GPU.
      if (!renderer.empty()) {
        errno = ENODEV;
        return -1;
      }
      renderer = dri / name;
    }
    if (error || renderer.empty()) {
      errno = error ? error.value() : ENODEV;
      return -1;
    }

    // GBM/EGL needs rendering and PRIME import, not modesetting ownership.
    // A primary-node fallback can seize DRM master while KWin is suspended;
    // render nodes cannot become master. Keep the fd out of spawned helpers.
    return open(renderer.c_str(), O_RDWR | O_CLOEXEC);
  }
}  // namespace cuda

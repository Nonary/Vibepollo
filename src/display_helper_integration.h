/**
 * @file src/display_helper_integration.h
 * @brief Cross-platform wrapper for display helper integration. On Windows, routes to the IPC helper; on other platforms, no-ops.
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <display_device/types.h>
#include <optional>

#ifdef _WIN32
  // Bring in the Windows implementation in the correct namespace
  #include "src/platform/windows/display_helper_integration.h"

namespace display_helper_integration {
  // On Windows, we exclusively use the helper and suppress in-process fallback.
  inline bool suppress_fallback() {
    return true;
  }

  // Enumerate display devices as a JSON string suitable for API responses.
  // Implemented in the Windows backend.
  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail);
}  // namespace display_helper_integration

#else

namespace display_helper_integration {
  // Non-Windows: No-op implementations that allow callers to fallback to in-process logic
  inline bool apply(const DisplayApplyRequest &) {
    return false;
  }

  inline bool revert() {
    return false;
  }

  inline bool export_golden_restore() {
    return false;
  }

  inline bool reset_persistence() {
    return false;
  }

  inline bool suppress_fallback() {
    return false;
  }

  inline std::string enumerate_devices_json(
    [[maybe_unused]] display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  ) {
    return "[]";
  }

  inline std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    [[maybe_unused]] display_device::DeviceEnumerationDetail detail = display_device::DeviceEnumerationDetail::Minimal
  ) {
    return std::nullopt;
  }
}  // namespace display_helper_integration

#endif

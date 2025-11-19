#include "display_helper_coordinator.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "virtual_display.h"

namespace platf::display_helper {
  Coordinator &Coordinator::instance() {
    static Coordinator instance;
    return instance;
  }

  std::optional<display_device::EnumeratedDeviceList> Coordinator::enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  ) {
    return display_helper_integration::enumerate_devices(detail);
  }

  std::string Coordinator::enumerate_devices_json() {
    return display_helper_integration::enumerate_devices_json();
  }

  std::optional<std::string> Coordinator::resolve_virtual_display_device_id() {
    return VDISPLAY::resolveAnyVirtualDisplayDeviceId();
  }

  void Coordinator::set_virtual_display_watchdog_enabled(bool enable) {
    VDISPLAY::setWatchdogFeedingEnabled(enable);
  }
}  // namespace platf::display_helper

#endif

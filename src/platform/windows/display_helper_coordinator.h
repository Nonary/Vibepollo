#pragma once

#include <display_device/types.h>

#include <optional>
#include <string>

namespace platf::display_helper {
  class Coordinator {
   public:
    static Coordinator &instance();

    std::optional<display_device::EnumeratedDeviceList> enumerate_devices();
    std::string enumerate_devices_json();
    std::optional<std::string> resolve_virtual_display_device_id();
    void set_virtual_display_watchdog_enabled(bool enable);
  };
}  // namespace platf::display_helper

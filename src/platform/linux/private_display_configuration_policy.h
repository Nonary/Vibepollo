#pragma once

#include <cmath>
#include <string_view>

namespace platf::linux_private_display::configuration_policy {
  // kscreen-doctor can report a rejected SetConfigOperation and still exit 0.
  inline bool command_succeeded(bool exited_successfully, std::string_view out, std::string_view err) {
    constexpr std::string_view rejection = "applying config failed";
    return exited_successfully && out.find(rejection) == out.npos && err.find(rejection) == err.npos;
  }

  inline bool usable_current_mode(bool connected, bool enabled, unsigned width, unsigned height, double refresh, double scale) {
    return connected && enabled && width > 0 && height > 0 &&
           std::isfinite(refresh) && refresh > 0 && std::isfinite(scale) && scale > 0;
  }
}  // namespace platf::linux_private_display::configuration_policy

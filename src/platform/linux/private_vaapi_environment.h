/** @file Keep the SteamOS host's private VAAPI driver out of launched applications. */
#pragma once

#include "src/boost_process_shim.h"

#include <string_view>

namespace platf::linux_private_vaapi {
  inline boost_process_shim::environment child_environment(
    const boost_process_shim::environment &source,
    const char *daemon_marker,
    const char *daemon_driver_path,
    const char *daemon_driver_name
  ) {
    if (!daemon_marker || std::string_view {daemon_marker} != "1") {
      return source;
    }

    boost_process_shim::environment result;
    for (const auto &entry : source) {
      const auto &name = entry.get_name();
      const auto value = entry.to_string();
      if (name == "VIBEPOLLO_PRIVATE_VAAPI" ||
          (name == "LIBVA_DRIVERS_PATH" && daemon_driver_path && value == daemon_driver_path) ||
          (name == "LIBVA_DRIVER_NAME" && daemon_driver_name && value == daemon_driver_name)) {
        continue;
      }
      result[name] = value;
    }
    return result;
  }
}  // namespace platf::linux_private_vaapi

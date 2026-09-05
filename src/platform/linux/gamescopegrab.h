/**
 * @file src/platform/linux/gamescopegrab.h
 * @brief Public routing surface for Gamescope PipeWire capture.
 */
#pragma once

#include "src/platform/common.h"

#include <memory>
#include <string>
#include <vector>

namespace video {
  struct config_t;
}

namespace platf {
  bool gamescope_available();
  bool gamescope_capture_selected();
  std::vector<std::string> gamescope_display_names();
  std::shared_ptr<display_t> gamescope_display(
    mem_type_e hwdevice_type,
    const std::string &display_name,
    const video::config_t &config
  );
}  // namespace platf

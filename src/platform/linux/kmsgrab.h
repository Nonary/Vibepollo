/** @file Declarations for capture of card-qualified Linux DRM connectors. */
#pragma once

#include "src/platform/common.h"

namespace platf {
  std::vector<std::string> kms_capture_targets(mem_type_e type);
  std::shared_ptr<display_t> kms_display(mem_type_e type, const std::string &name, const video::config_t &config);
}

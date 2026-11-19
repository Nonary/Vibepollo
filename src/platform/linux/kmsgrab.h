/** @file Declarations for capture of card-qualified Linux DRM connectors. */
#pragma once

#include "capture_target.h"
#include "src/platform/common.h"

namespace platf {
  std::vector<std::string> kms_capture_targets(mem_type_e type, const capture_deadline_t &limits = {});
  /** Public shortcut order. KMS entries retain their card identity when it is the selected backend. */
  std::vector<std::string> display_switch_targets(mem_type_e type, display_switch_source_e &source, const capture_deadline_t &limits);
  std::shared_ptr<display_t> kms_display(mem_type_e type, const std::string &name, const video::config_t &config, const capture_deadline_t &limits = {});
}

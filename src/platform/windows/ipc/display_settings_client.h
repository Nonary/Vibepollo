/**
 * @file src/platform/windows/ipc/display_settings_client.h
 * @brief Client helper to send display apply/revert commands to the helper process.
 */
#pragma once

#ifdef _WIN32

#include <string>

namespace platf::display_helper_client {
  // Send APPLY with JSON payload (SingleDisplayConfiguration)
  bool send_apply_json(const std::string &json);

  // Send REVERT (no payload)
  bool send_revert();
}

#endif


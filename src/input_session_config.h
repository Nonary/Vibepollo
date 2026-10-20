/**
 * @file src/input_session_config.h
 * @brief Settings owned by one input session rather than the shared capture.
 */
#pragma once

namespace input {
  struct guide_button_config_t {
    int key = 0;
    int controller = 0;
  };
}  // namespace input

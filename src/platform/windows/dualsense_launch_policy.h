// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

namespace proc::dualsense_launch {
  enum class decision {
    resume,
    wait,
    timed_out
  };

  // A launch request must return before RTSP can deliver a controller. Keep
  // that application's lifetime alive, but bound the wait for HID and audio.
  inline decision evaluate(bool waiting_for_controller, bool ready, std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now) {
    if (!waiting_for_controller) {
      return decision::resume;
    }
    if (now >= deadline) {
      return decision::timed_out;
    }
    return ready ? decision::resume : decision::wait;
  }
}  // namespace proc::dualsense_launch

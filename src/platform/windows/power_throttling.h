/**
 * @file src/platform/windows/power_throttling.h
 * @brief Process power-throttling policy for the capture and encode processes.
 */
#pragma once

#include <windows.h>

namespace platf {

  /**
   * @brief Opt the current process out of EcoQoS and timer-resolution throttling.
   * @details Windows 11 may run a process without a visible window at reduced CPU
   *          QoS (efficiency cores, lower clocks) and may ignore its timer
   *          resolution request. The host and the WGC helper are windowless while
   *          streaming, and both effects stretch capture pacing and encoder wakeups.
   * @param enable true requests full QoS; false returns control to the system default.
   * @return true if the requested policy was applied.
   */
  inline bool set_process_high_qos(bool enable) {
    PROCESS_POWER_THROTTLING_STATE state {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // A policy bit set in ControlMask and clear in StateMask turns that
    // throttling off. An empty ControlMask hands the policy back to Windows.
    state.ControlMask = enable ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
    state.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
      return true;
    }
    if (!enable) {
      return false;
    }

    // Windows 10 predates the timer-resolution policy and rejects the whole
    // request when that bit is present.
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
  }

}  // namespace platf

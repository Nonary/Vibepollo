/**
 * @file src/platform/windows/local_input_monitor.h
 * @brief Tracks genuine local (physical) input activity on the host, as
 *        distinct from input Sunshine/Apollo itself injects for a connected
 *        Moonlight/Artemis client.
 *
 * This exists to answer one question: "is a person physically at this
 * machine right now?" so that @ref nvhttp::launch can decline (or the
 * caller can otherwise gate on) a new/competing connection while someone is
 * locally using the PC, rather than silently taking over their display or
 * input.
 *
 * Mouse and keyboard are tracked via low-level hooks (WH_MOUSE_LL /
 * WH_KEYBOARD_LL) rather than GetLastInputInfo, specifically because
 * GetLastInputInfo cannot distinguish physical input from input injected via
 * SendInput - and Sunshine's own Windows input backend (see
 * platform/windows/input.cpp) injects all remote keyboard/mouse input via
 * SendInput. The low-level hooks expose an LLKHF_INJECTED / LLMHF_INJECTED
 * flag per event that GetLastInputInfo does not, which is what makes
 * filtering out our own injected input possible at all.
 *
 * Controller (XInput) activity has no equivalent injection flag exposed to
 * user-mode polling code. To compensate, controller changes are only ever
 * counted as physical input while no RTSP session is active at all (i.e. no
 * ViGEm virtual pad could currently be attached) - see
 * local_input_monitor.cpp's poll_controllers_once() for the detail and the
 * resulting limitation (controller activity is effectively ignored while a
 * session is already live; only mouse/keyboard count then).
 */
#pragma once

#ifdef _WIN32

  #include "src/platform/common.h"

  #include <chrono>
  #include <memory>

namespace local_input_monitor {

  /**
   * @brief Start the local input monitor.
   *
   * Spawns a dedicated thread that runs a Windows message loop hosting the
   * low-level keyboard/mouse hooks, plus a lightweight poll loop for XInput
   * controller state. Safe to call once per process lifetime; a second call
   * while already running logs a warning and returns a no-op guard.
   *
   * @return RAII guard that uninstalls the hooks and stops the thread when
   *         destroyed.
   */
  std::unique_ptr<platf::deinit_t>
    start();

  /**
   * @brief Time elapsed since the most recent confirmed physical input.
   *
   * "Confirmed physical" means: for mouse/keyboard, the low-level hook saw
   * the event without the OS-reported injected flag set. For controllers,
   * see @ref controller_signal_trusted for when this is meaningful at all.
   *
   * @return Milliseconds since the last such event, or
   *         std::chrono::milliseconds::max() if none has been observed yet
   *         (e.g. the monitor was just started, or is not running at all on
   *         this build/platform).
   */
  std::chrono::milliseconds
    time_since_local_input();

  /**
   * @brief Convenience wrapper: true if local input was seen within
   *        @p threshold.
   */
  bool
    locally_active(std::chrono::milliseconds threshold);

}  // namespace local_input_monitor

#endif  // _WIN32

/**
 * @file src/platform/macos/misc.h
 * @brief Miscellaneous declarations for macOS platform.
 */
#pragma once

// standard includes
#include <functional>
#include <vector>

// platform includes
#include <CoreGraphics/CoreGraphics.h>

namespace platf {
  bool is_screen_capture_allowed();

  /**
   * @brief Set up this process's AppKit session once. Call on the main thread.
   * @details Display configuration and display-mode queries only work in an AppKit session.
   */
  void ensure_appkit_session();

  /**
   * @brief Run the AppKit event loop on the main thread until `should_exit` returns true.
   * @details Delivers menu bar events and display reconfiguration notifications. `should_exit`
   *          is checked at least twice a second.
   */
  void run_main_event_loop(const std::function<bool()> &should_exit);
}

namespace dyn {
  typedef void (*apiproc)();

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn

/**
 * @file src/platform/macos/misc.h
 * @brief Miscellaneous declarations for macOS platform.
 */
#pragma once

// standard includes
#include <chrono>
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

  /**
   * @brief Whether Open at Login is on.
   * @details launchd then runs Vibepollo from login, and restarts it if it crashes. It's listed
   *          under Allow in the Background in System Settings > General > Login Items.
   */
  bool opens_at_login();

  /**
   * @brief Whether launchd started this process for Open at Login.
   */
  bool is_login_agent();

  /**
   * @brief Turn Open at Login on or off.
   * @details Turning it on starts the login agent, which then takes over from this process.
   *          Turning it off stops the login agent, so if this is it, a new copy of Vibepollo
   *          carries on. If the user turned Vibepollo off in System Settings, only they can turn
   *          it back on, so this opens Login Items for them.
   * @return `true` if Open at Login is now `enabled`.
   */
  bool set_opens_at_login(bool enabled);

  /**
   * @brief If Open at Login is on and this isn't the login agent, start the agent instead.
   * @details Keeps Vibepollo under launchd's care however it's opened.
   * @return `true` if this process should exit and leave it to the agent.
   */
  bool defer_to_login_agent();

  /**
   * @brief Make sure no other copy of Vibepollo runs alongside this one.
   * @details Waits for another copy to quit, as it does when handing over to or from the login
   *          agent. The login agent asks the other copy to quit first.
   * @param timeout How long to wait for another copy to quit.
   * @return `false` if another copy is still running.
   */
  bool acquire_instance_lock(std::chrono::milliseconds timeout);

  /**
   * @brief Turn the displays on if they're asleep, as a keypress would, and wait for one to come on.
   * @details A client can connect while the Mac sleeps. Its network traffic only brings the Mac into
   *          a "dark wake" with every display off, where captures get no frames. Returns right
   *          away on a Mac with no display connected.
   * @param timeout How long to wait for a display to come on.
   * @return `true` if a display is on.
   */
  bool wake_displays(std::chrono::milliseconds timeout = std::chrono::seconds {5});

  /**
   * @brief Run `handler` whenever the Mac is about to sleep, before letting it sleep.
   * @details For ending streams properly: otherwise a client just sees its stream freeze. The
   *          handler runs on a background queue. Only the first call registers a handler.
   */
  void on_system_will_sleep(std::function<void()> handler);

  /**
   * @brief Run `handler` whenever a MacBook's lid goes from open to closed.
   * @details The handler runs on a background queue. Only the first call registers a handler.
   */
  void on_lid_closed(std::function<void()> handler);
}

namespace dyn {
  typedef void (*apiproc)();

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn

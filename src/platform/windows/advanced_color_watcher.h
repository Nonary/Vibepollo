/**
 * @file src/platform/windows/advanced_color_watcher.h
 * @brief Reports advanced-color (HDR) changes on one monitor through DisplayInformation.
 */
#pragma once

// standard includes
#include <functional>
#include <memory>
#include <thread>

// platform includes
#include <winsock2.h>
#include <windows.h>

// lib includes
#include <winrt/base.h>

namespace platf::dxgi {

  /**
   * @brief Calls back when Windows reports an advanced-color change on one monitor.
   * @details DisplayInformation delivers AdvancedColorInfoChanged only to a thread
   *          that has a DispatcherQueue, so the watcher runs a small STA thread with
   *          its own queue and message loop. The callback runs on that thread.
   */
  class advanced_color_watcher_t {
  public:
    /**
     * @brief Subscribe to the monitor's advanced-color changes.
     * @param monitor The monitor to watch.
     * @param on_change Called on the watcher thread for every reported change.
     * @return The watcher, or nullptr when Windows cannot deliver the notification
     *         (DisplayInformation for a monitor needs Windows 11 22H2) or it failed.
     */
    static std::unique_ptr<advanced_color_watcher_t> start(HMONITOR monitor, std::function<void()> on_change);

    ~advanced_color_watcher_t();

    advanced_color_watcher_t(const advanced_color_watcher_t &) = delete;
    advanced_color_watcher_t &operator=(const advanced_color_watcher_t &) = delete;

  private:
    advanced_color_watcher_t() = default;

    winrt::handle _stop;  ///< Set to end the watcher thread.
    std::thread _thread;
  };

}  // namespace platf::dxgi

/**
 * @file src/platform/windows/ctm_bridge.h
 * @brief Service-lifetime supervisor for the external CTM bridge agent (ctm-usbip.exe).
 */
#pragma once

namespace ctm_bridge {
  /**
   * @brief Start the CTM bridge supervisor thread.
   *
   * Idempotent; a no-op if already running. The supervisor honors config::ctm at
   * runtime: while ctm.enable is true it keeps ctm-usbip.exe alive, and while it is
   * false it terminates any managed instance. Safe to call even when the feature is
   * disabled (the thread simply idles until enabled).
   */
  void start_watchdog();

  /**
   * @brief Stop the supervisor thread and terminate the managed agent (best-effort).
   */
  void stop_watchdog();
}  // namespace ctm_bridge

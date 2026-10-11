/**
 * @file src/host_monitor.h
 * @brief Eclipse: turn the host's physical monitor(s) off while a client streams, when the client asks.
 *
 * WHY. Streaming from the virtual display leaves the host's own monitor showing the desktop -- or, after a
 * display change, waking up mid-session. Whether that is wanted is the CLIENT's call (Eclipse 2026-10-04:
 * "Host monitor while streaming" = always off / always on / choose at launch), so it is a per-launch request,
 * not a host setting: /launch and /resume carry umbraMonitorOff=1, which also forces the virtual display (a
 * stream of a monitor that is then switched off would show nothing).
 *
 * WHAT. While a requested stream runs, every PHYSICAL display path is deactivated, but only while a VIRTUAL
 * display carries the desktop -- never a desktop with no display at all. The topology is saved to the display
 * database (SDC_SAVE_TO_DATABASE) because without that Windows re-applies its stored layout and the panel
 * comes back mid-session (measured 2026-09-21, three times in five minutes). A watchdog re-checks every few
 * seconds and logs every time the panel came back. When the last client disconnects (pause or end), the
 * panel is restored -- EXTEND, verified, with a wake fallback -- and that state is saved in turn.
 *
 * Ported from the rig's dispwake.exe + apollo-display.cmd hooks (Eclipse xlink/dispwake), which did this for
 * every stream; classification of a path as virtual is unchanged: indirect output technology, or the SudoVDA
 * EDID vendor "SMK".
 */
#pragma once

namespace host_monitor {
  /** serverinfo `UmbraCaps` bit: /launch and /resume understand umbraMonitorOff (Windows). */
  constexpr int CAP_MONITOR_OFF = 16;

  /** Can this platform switch the physical monitor off? */
  bool supported();

  /** The first client of a stream connected; @p monitor_off = it asked for the physical monitor(s) off. */
  void session_started(bool monitor_off);

  /** The last client disconnected (pause or end): restore whatever session_started turned off. */
  void sessions_stopped();
}  // namespace host_monitor

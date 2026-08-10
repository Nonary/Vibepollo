#pragma once

namespace display_helper::v2::shell_refresh_policy {
  // The helper owns bounded WM_SETTINGCHANGE/WM_DISPLAYCHANGE broadcasts.
  // Do not add an unbounded system-parameter broadcast ahead of the capture gate.
  bool allows_unbounded_system_parameter_broadcast();

  // HWND_BROADCAST applies its timeout to every top-level window. Dispatch the
  // bounded notifications before releasing capture, but never wait for them on
  // the verification/capture-gate thread.
  bool defers_window_broadcasts_from_capture_gate();
}  // namespace display_helper::v2::shell_refresh_policy

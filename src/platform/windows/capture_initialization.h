#pragma once

namespace platf::dxgi {
  // platf::display() opens several candidate capture backends on this thread.
  // Report only the final failed backend's display-specific reason; encoder,
  // D3D device, IPC and access failures must not authorize topology changes.
  inline thread_local bool capture_initialization_display_unavailable = false;
}

#pragma once
#include "frame_push_io.h"
#ifdef _WIN32
#include <string>
namespace platf::frame_push {
  // Opens only the caller-selected existing control interface. It creates no
  // device, service or driver; production use is behind the default-off
  // capture build option and live-registration checks.
  std::shared_ptr<AsyncIo> open_frame_push_windows_io(const std::wstring &device_path);
}
#endif

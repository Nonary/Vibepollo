#pragma once

#include <windows.h>

namespace VDISPLAY::legacy {
  LONG changeDisplaySettings(const wchar_t *deviceName, int width, int height, int refresh_rate);
  LONG changeDisplaySettings2(const wchar_t *deviceName, int width, int height, int refresh_rate, bool apply_isolated = false);
  bool getDisplayHDRByName(const wchar_t *displayName);
  bool setDisplayHDRByName(const wchar_t *displayName, bool enableAdvancedColor);
}  // namespace VDISPLAY::legacy

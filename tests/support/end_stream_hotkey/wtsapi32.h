#pragma once
#include "windows.h"

inline DWORD WTSGetActiveConsoleSessionId() {
  return hotkey_test_api::state.console;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ipc.h"

#include <string>

namespace dualsense_haptics {
  struct launcher_config {
    wchar_t mapping_name[128] {};
    wchar_t directory[32768] {};
  };

  inline bool under_directory(const std::wstring &path, const std::wstring &directory) {
    return !directory.empty() && path.size() > directory.size() &&
           _wcsnicmp(path.c_str(), directory.c_str(), directory.size()) == 0 &&
           (path[directory.size()] == L'\\' || path[directory.size()] == L'/');
  }
}  // namespace dualsense_haptics

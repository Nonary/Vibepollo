// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ipc.h"

namespace dualsense_haptics {
  struct bootstrap_config {
    wchar_t mapping_name[128];
    std::uint64_t entry;
    std::array<BYTE, 14> original;
    volatile LONG ready;
    volatile LONG gate;
  };
}  // namespace dualsense_haptics

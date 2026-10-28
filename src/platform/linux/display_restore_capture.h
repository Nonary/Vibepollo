/** @file Noninteractive capture discovery for bounded desktop restoration. */
#pragma once

#include "src/platform/common.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace platf {
  /**
   * Observe the selected backend without opening portal consent or capture
   * sessions. Empty results mean readiness could not be established safely.
   */
  std::vector<std::string> display_names_for_restore(
    mem_type_e hwdevice_type,
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool()> &allowed
  );
}

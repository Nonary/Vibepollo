#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

namespace display_helper::v2 {
  class WinPlatformWorkarounds final : public IPlatformWorkarounds {
  public:
    void blank_hdr_states(std::chrono::milliseconds delay) override;
    void refresh_shell() override;
  };
}  // namespace display_helper::v2

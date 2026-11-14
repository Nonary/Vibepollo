#pragma once

#include <cstdint>

namespace input::scroll_policy {
  // Each client owns its own remainder. A disconnect discards only that
  // client's partial detent rather than affecting another client's scroll.
  constexpr int accumulate(std::int32_t &remainder, int delta) noexcept {
    remainder += delta;
    const auto whole = remainder / 120;
    remainder %= 120;
    return whole * 120;
  }

  // A fractional detent retained for VHF was never submitted to the driver.
  // If the active backend changes to high-resolution input, deliver it with
  // this client's next packet rather than silently discarding it.
  constexpr int pass_through(std::int32_t &remainder, int delta) noexcept {
    const auto total = remainder + delta;
    remainder = 0;
    return total;
  }
}

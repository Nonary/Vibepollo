#pragma once

#include <chrono>

namespace display_helper::v2::timing {
  // Bound the user-visible stream-start delay to roughly the legacy helper's
  // practical startup window. V2 may continue best-effort stabilization after
  // this deadline, but capture must not remain black through the full recovery
  // and retry envelope.
  inline constexpr auto kApplyStartupBudget = std::chrono::seconds(15);

  // The verification producer resolves its future at the startup deadline.
  // Give the capture consumer only a small scheduling margin beyond it.
  inline constexpr auto kApplyGateConsumerSlack = std::chrono::seconds(1);
}  // namespace display_helper::v2::timing

#pragma once

#include <chrono>

namespace display_helper::v2::timing {
  // Preserve the full apply/verification envelope for WebRTC, recovery, and
  // other callers that are not racing an RTSP client's first-video timeout.
  inline constexpr auto kApplyStartupBudget = std::chrono::seconds(15);

  // Moonlight clients can abandon an RTSP session after roughly ten seconds
  // without video. APPLY and its verification gate share this shorter budget,
  // leaving time for capture initialization and the first encoded frame.
  inline constexpr auto kStreamStartApplyBudget = std::chrono::seconds(8);

  // The RTSP verification producer resolves its future at the stream-start
  // deadline. Give the capture consumer only a small scheduling margin beyond it.
  inline constexpr auto kApplyGateConsumerSlack = std::chrono::seconds(1);
}  // namespace display_helper::v2::timing

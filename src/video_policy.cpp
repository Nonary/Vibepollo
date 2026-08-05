#include "video_policy.h"

#include <numeric>

namespace video::policy {
  rational_t framerate_x100_to_rational(std::int32_t value) {
    if (value % 2997 == 0) return {(value / 2997) * 30000, 1001};
    if (value == 2397 || value == 2398) return {24000, 1001};
    const auto divisor = std::gcd(value, 100);
    return {value / divisor, 100 / divisor};
  }

  std::optional<std::string> select_encoder(
    std::span<const std::string_view> preference,
    encoder_requirements_t requirements,
    const encoder_capability_provider_t &provider
  ) {
    for (const auto name : preference) {
      const auto caps = provider.capabilities(name);
      if (caps.available && (!requirements.hdr || caps.hdr) && (!requirements.yuv444 || caps.yuv444)) {
        return std::string(name);
      }
    }
    return std::nullopt;
  }
}  // namespace video::policy

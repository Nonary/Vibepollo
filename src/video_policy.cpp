#include "video_policy.h"

#include <algorithm>
#include <cctype>
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

  std::optional<std::string> select_preferred_virtual_output(
    std::string_view configured_output,
    std::span<const std::string> active_virtual_outputs,
    std::span<const std::string> all_virtual_outputs
  ) {
    const auto equals_case_insensitive = [](std::string_view left, std::string_view right) {
      return left.size() == right.size() && std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [](const char lhs, const char rhs) {
          return std::tolower(static_cast<unsigned char>(lhs)) ==
                 std::tolower(static_cast<unsigned char>(rhs));
        }
      );
    };
    const auto find_configured = [&](std::span<const std::string> outputs) -> std::optional<std::string> {
      if (configured_output.empty()) {
        return std::nullopt;
      }
      const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const std::string &output) {
        return equals_case_insensitive(output, configured_output);
      });
      return found == outputs.end() ? std::nullopt : std::optional<std::string> {*found};
    };

    if (auto configured = find_configured(active_virtual_outputs)) {
      return configured;
    }
    if (auto configured = find_configured(all_virtual_outputs)) {
      return configured;
    }
    if (!active_virtual_outputs.empty()) {
      return active_virtual_outputs.front();
    }
    if (!all_virtual_outputs.empty()) {
      return all_virtual_outputs.front();
    }
    return std::nullopt;
  }
}  // namespace video::policy

#include "video_policy.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace video::policy {
  namespace {
    bool equals_case_insensitive(std::string_view left, std::string_view right) {
      return left.size() == right.size() && std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [](const char lhs, const char rhs) {
          return std::tolower(static_cast<unsigned char>(lhs)) ==
                 std::tolower(static_cast<unsigned char>(rhs));
        }
      );
    }
  }  // namespace

  bool may_apply_process_display_preference(const capture_selection_e selection) {
    return selection == capture_selection_e::process_preferred;
  }

  std::chrono::milliseconds display_retry_delay(const std::size_t consecutive_failures) {
    constexpr auto initial_delay = std::chrono::milliseconds {50};
    constexpr auto maximum_delay = std::chrono::milliseconds {2000};
    constexpr std::size_t maximum_shift = 6;
    const auto shift = std::min(consecutive_failures, maximum_shift);
    const auto multiplier = static_cast<std::chrono::milliseconds::rep>(std::size_t {1} << shift);
    return std::min(initial_delay * multiplier, maximum_delay);
  }

  bool should_log_display_retry(const std::size_t consecutive_failures) {
    return consecutive_failures == 0 || (consecutive_failures & (consecutive_failures - 1)) == 0;
  }

  std::optional<std::string> select_manual_display_output(
    const capture_selection_e selection,
    const int requested_index,
    const std::span<const std::string> display_names
  ) {
    if (selection != capture_selection_e::process_preferred ||
        requested_index < 0 || display_names.empty()) {
      return std::nullopt;
    }

    const auto index = std::clamp(requested_index, 0, static_cast<int>(display_names.size()) - 1);
    return display_names[static_cast<std::size_t>(index)];
  }

  std::optional<int> resolve_display_output(
    const std::string_view output_identity,
    const std::span<const std::string> display_names
  ) {
    const auto found = std::find_if(display_names.begin(), display_names.end(), [&](const std::string &name) {
      return equals_case_insensitive(name, output_identity);
    });
    if (found == display_names.end()) {
      return std::nullopt;
    }
    return static_cast<int>(std::distance(display_names.begin(), found));
  }

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

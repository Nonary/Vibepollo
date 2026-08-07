#include "rtx_hdr_policy.h"

#include <algorithm>
#include <cctype>

namespace platf::rtx_hdr::policy {
  namespace {
    bool has_tuning_values(const profile_values_t &values) { return values.contrast || values.saturation || values.middle_gray || values.peak_brightness; }
    profile_source_e tuning_source(const resolved_profile_t &resolved, const overrides_t &overrides) {
      if (overrides.contrast || overrides.saturation || overrides.middle_gray || overrides.peak_brightness) return profile_source_e::config;
      if (has_tuning_values(resolved.application)) return profile_source_e::application;
      if (has_tuning_values(resolved.global)) return profile_source_e::global;
      return profile_source_e::config;
    }
    std::string normalize_path(std::string_view value) {
      std::string result(value);
      std::replace(result.begin(), result.end(), '/', '\\');
      while (!result.empty() && result.back() == '\\') result.pop_back();
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }
    std::string basename(std::string_view value) {
      const auto path = normalize_path(value); const auto position = path.find_last_of('\\');
      return position == std::string::npos ? path : path.substr(position + 1);
    }
  }

  runtime_values_t materialize(const resolved_profile_t &resolved, const runtime_values_t &config, const overrides_t &overrides) {
    runtime_values_t values;
    if (!overrides.enable) return values;
    if (!config.enabled) { values.source = profile_source_e::config; return values; }
    values.enabled = true;
    values.contrast = overrides.contrast ? config.contrast : resolved.application.contrast.value_or(resolved.global.contrast.value_or(config.contrast));
    values.saturation = overrides.saturation ? config.saturation : resolved.application.saturation.value_or(resolved.global.saturation.value_or(config.saturation));
    values.middle_gray = overrides.middle_gray ? config.middle_gray : resolved.application.middle_gray.value_or(resolved.global.middle_gray.value_or(config.middle_gray));
    values.sdr_brightness = config.sdr_brightness;
    values.peak_brightness = overrides.peak_brightness ? config.peak_brightness : resolved.application.peak_brightness.value_or(resolved.global.peak_brightness.value_or(config.peak_brightness));
    values.source = tuning_source(resolved, overrides);
    return values;
  }

  runtime_values_t desktop_values(const runtime_values_t &config, bool runtime_enabled) {
    runtime_values_t values;
    if (!runtime_enabled || !config.enabled) return values;
    values.sdr_brightness = config.sdr_brightness; values.peak_brightness = config.peak_brightness; values.source = profile_source_e::config;
    return values;
  }
  std::optional<bool> decode_activation(std::optional<std::uint32_t> flags, std::optional<std::uint32_t> enabled) {
    if (flags && *flags != 0) return true;
    if (enabled) return *enabled <= 1 ? std::optional<bool>{*enabled != 0} : std::nullopt;
    if (flags) return false;
    return std::nullopt;
  }
  std::optional<int> decode_percent_units(std::uint32_t raw) { return raw <= 200 ? std::optional<int>{static_cast<int>(raw)} : std::nullopt; }
  float sdr_brightness_to_white_nits(int brightness) { return 100.0f + static_cast<float>(std::clamp(brightness, 0, 100)); }
  bool playnite_foreground_matches(std::string_view active_id, std::string_view status_id, std::string_view status_exe, std::string_view install_dir, std::string_view foreground_exe) {
    if (foreground_exe.empty() || status_id.empty() || (!active_id.empty() && active_id != status_id)) return false;
    const auto foreground = normalize_path(foreground_exe); const auto status = normalize_path(status_exe); const auto directory = normalize_path(install_dir);
    if ((!status.empty() && foreground == status) || (!basename(foreground).empty() && basename(foreground) == basename(status))) return true;
    return !directory.empty() && foreground.size() > directory.size() && foreground.compare(0, directory.size(), directory) == 0 && foreground[directory.size()] == '\\';
  }
}

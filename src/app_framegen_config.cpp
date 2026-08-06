/**
 * @file src/app_framegen_config.cpp
 * @brief Pure parsing of per-application frame-generation settings.
 */

#include "app_framegen_config.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace proc::app_config {
  std::string normalize_framegen_provider(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
      if (std::isalnum(ch)) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    if (normalized == "nvidia" || normalized == "smoothmotion" || normalized == "nvidiasmoothmotion") {
      return "nvidia-smooth-motion";
    }
    if (normalized == "game" || normalized == "gameprovided" || normalized == "gameprovider") {
      return "game-provided";
    }
    return "lossless-scaling";
  }

  std::optional<framegen_t> parse_framegen_json(std::string_view app_json) {
    try {
      const auto app = nlohmann::json::parse(app_json);
      if (!app.is_object()) {
        return std::nullopt;
      }

      framegen_t result;
      const bool requested_gen1 = app.value(
        "gen1-framegen-fix",
        app.value("dlss-framegen-capture-fix", false));
      const bool requested_gen2 = app.value("gen2-framegen-fix", false);
      result.lossless_scaling_framegen = app.value("lossless-scaling-framegen", false);
      result.provider = normalize_framegen_provider(app.value("frame-generation-provider", std::string {"lossless-scaling"}));

      bool explicitly_off = false;
      if (const auto mode_it = app.find("frame-generation-mode"); mode_it != app.end() && mode_it->is_string()) {
        auto mode = mode_it->get<std::string>();
        mode.erase(mode.begin(), std::find_if(mode.begin(), mode.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        mode.erase(std::find_if(mode.rbegin(), mode.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), mode.end());
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        explicitly_off = mode == "off" || mode == "none" || mode == "disabled";
        if (explicitly_off) {
          result.provider = "lossless-scaling";
          result.lossless_scaling_framegen = false;
        } else {
          result.provider = normalize_framegen_provider(mode);
          result.lossless_scaling_framegen = result.provider == "lossless-scaling";
          result.enabled = true;
        }
      } else {
        result.enabled =
          result.lossless_scaling_framegen ||
          result.provider == "game-provided" ||
          result.provider == "nvidia-smooth-motion" ||
          requested_gen1 ||
          requested_gen2;
      }

      result.gen1_capture_fix = !explicitly_off && (requested_gen1 || requested_gen2);
      result.gen2_capture_fix = false;
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }
}  // namespace proc::app_config

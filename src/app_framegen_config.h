/**
 * @file src/app_framegen_config.h
 * @brief Pure parsing of per-application frame-generation settings.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace proc::app_config {
  struct framegen_t {
    bool enabled = false;
    bool gen1_capture_fix = false;
    bool gen2_capture_fix = false;
    bool lossless_scaling_framegen = false;
    std::string provider {"lossless-scaling"};
  };

  std::string normalize_framegen_provider(std::string_view value);
  std::optional<framegen_t> parse_framegen_json(std::string_view app_json);
}  // namespace proc::app_config

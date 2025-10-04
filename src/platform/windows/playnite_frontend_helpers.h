/**
 * @file src/platform/windows/playnite_frontend_helpers.h
 * @brief Pure helpers to build Playnite frontend JSON responses for unit testing.
 */
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace platf::playnite::frontend {

  /**
   * Build the JSON payload returned by the Playnite status endpoint.
   * Inputs are provided explicitly to keep this function pure and unit-testable.
   */
  inline nlohmann::json build_status_json(bool enabled, bool active, const std::filesystem::path &extensions_dir, bool /*session_required_unused*/, bool installed, bool playnite_running) {
    nlohmann::json out;
    out["enabled"] = enabled;
    out["active"] = active;
    out["extensions_dir"] = extensions_dir.string();
    out["installed"] = installed;
    out["playnite_running"] = playnite_running;
    return out;
  }

}  // namespace platf::playnite::frontend

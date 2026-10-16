/**
 * @file src/platform/linux/private_display_emergency_policy.h
 * @brief Physical monitor candidates for explicit user-requested emergency recovery.
 */
#pragma once

#include "private_display_snapshot_policy.h"

#include <cmath>
#include <string>
#include <vector>

namespace platf::linux_private_display::emergency_policy {
  template<typename Json>
  bool physical_active(const Json &configuration) {
    return snapshot_policy::valid(configuration) && std::ranges::any_of(configuration["outputs"], [](const auto &output) {
      return output.value("connected", false) && output.value("enabled", false) &&
             !mode_policy::managed_connector_name(output.value("name", std::string {}));
    });
  }

  /** Ordinary restore never enables a disabled monitor by guesswork. Only the killswitch may. */
  template<typename Json>
  std::vector<Json> physical_candidates(const Json &configuration) {
    std::vector<Json> candidates;
    if (!snapshot_policy::valid(configuration) || physical_active(configuration)) return candidates;
    for (const auto &output : configuration["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (!output.value("connected", false) || mode_policy::managed_connector_name(name)) continue;
      const auto modes = output.find("modes");
      if (modes == output.end() || !modes->is_array()) continue;
      const auto usable = [](const auto &mode) {
        try {
          if (!mode.is_object()) return false;
          const auto size = mode.value("size", Json::object());
          const auto rate = mode.value("refreshRate", 0.0);
          return snapshot_policy::safe_identifier(mode.value("id", std::string {})) &&
                 size.value("width", 0) > 0 && size.value("height", 0) > 0 &&
                 size.value("width", 0) <= 65536 && size.value("height", 0) <= 65536 &&
                 std::isfinite(rate) && rate > 0.0;
        } catch (...) {
          return false;
        }
      };
      auto selected = modes->end();
      // Prefer the monitor's advertised native mode, then its current mode.
      std::vector<std::string> preferred;
      if (const auto ids = output.find("preferredModes"); ids != output.end() && ids->is_array()) {
        for (const auto &id : *ids) {
          if (id.is_string()) preferred.push_back(id.template get<std::string>());
        }
      }
      preferred.push_back(output.value("currentModeId", std::string {}));
      for (const auto &id : preferred) {
        selected = std::ranges::find_if(*modes, [&](const auto &mode) {
          return usable(mode) && mode.value("id", std::string {}) == id;
        });
        if (selected != modes->end()) break;
      }
      if (selected == modes->end()) selected = std::ranges::find_if(*modes, usable);
      if (selected == modes->end()) continue;
      auto candidate = output;
      candidate["enabled"] = true;
      candidate["currentModeId"] = selected->value("id", std::string {});
      candidate["size"] = (*selected)["size"];
      candidate["scale"] = 1.0;
      candidate["rotation"] = 1;
      candidate["pos"] = {{"x", 0}, {"y", 0}};
      candidate["priority"] = 1;
      candidate["hdr"] = false;
      candidates.push_back(std::move(candidate));
    }
    return candidates;
  }
}  // namespace platf::linux_private_display::emergency_policy

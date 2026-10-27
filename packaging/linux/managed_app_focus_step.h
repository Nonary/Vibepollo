#pragma once

#include "src/managed_app_focus.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace managed_app_focus {
  // One poll of the focus loop; apply({}) only observes. A game window that
  // already has the foreground spends no attempt. The clock is read after the
  // blocking observation, so a delayed reply cannot admit an expired window.
  template<class Apply, class Now>
  bool focus_step(Apply &&apply, budget &focus, Now &&clock_now) {
    const nlohmann::json observed = apply(std::vector<std::string> {});
    const auto now = clock_now();
    if (!observed.is_object() || !observed.contains("windows") || !observed["windows"].is_array()) {
      return false;
    }
    std::vector<std::string> allowed;
    for (const auto &window : observed["windows"]) {
      if (window.is_string()) {
        const auto identity = window.get<std::string>();
        focus.observe(identity, now);
        if (focus.can_focus(identity, now)) {
          allowed.push_back(identity);
        }
      }
    }
    // The preferred eligible window comes first; any other window in front,
    // even one of the game's own process, is lost focus.
    if (allowed.empty() || observed.value("foreground", std::string {}) == allowed.front()) {
      return false;
    }
    const nlohmann::json applied = apply(allowed);
    if (!applied.is_object() || !applied.value("focused", false)) {
      return false;
    }
    focus.confirmed();
    return true;
  }
}  // namespace managed_app_focus

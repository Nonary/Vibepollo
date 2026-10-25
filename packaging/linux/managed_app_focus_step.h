#pragma once

#include "src/managed_app_focus.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace managed_app_focus {
  // One poll of the focus loop; apply({}) only observes. A game window that
  // already has the foreground spends no attempt.
  template<class Apply>
  bool focus_step(Apply &&apply, budget &focus, budget::clock::time_point now) {
    const nlohmann::json observed = apply(std::vector<std::string> {});
    if (!observed.is_object() || !observed.contains("windows") || !observed["windows"].is_array()) {
      return false;
    }
    std::vector<std::string> windows;
    for (const auto &window : observed["windows"]) {
      if (window.is_string()) {
        windows.push_back(window.get<std::string>());
        focus.observe(windows.back(), now);
      }
    }
    if (windows.empty() || !focus.can_focus(now)) {
      return false;
    }
    // The preferred window comes first; any other window in front, even one
    // of the game's own process, is lost focus.
    if (observed.value("foreground", std::string {}) == windows.front()) {
      return false;
    }
    std::vector<std::string> allowed;
    for (const auto &identity : windows) {
      if (focus.can_focus(identity, now)) {
        allowed.push_back(identity);
      }
    }
    if (allowed.empty()) {
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

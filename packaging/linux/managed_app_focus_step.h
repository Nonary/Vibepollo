#pragma once

#include "src/managed_app_focus.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace managed_app_focus {
  // One poll of the focus loop; apply({}) only observes. A game window that
  // already has the foreground spends no attempt.
  template<class Apply>
  bool focus_step(Apply &&apply, budget &focus, const std::vector<std::uint64_t> &pids, budget::clock::time_point now) {
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
    // The preferred window comes first; its launcher in front is lost focus.
    std::uint64_t preferred_pid = 0;
    const auto &preferred = windows.front();
    if (const auto separator = preferred.find(':'); separator != std::string::npos) {
      std::from_chars(preferred.data(), preferred.data() + separator, preferred_pid);
    }
    const auto foreground = observed.value("foreground_pid", std::uint64_t {0});
    if (foreground && foreground == preferred_pid && std::ranges::find(pids, foreground) != pids.end()) {
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

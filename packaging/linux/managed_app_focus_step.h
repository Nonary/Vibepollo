#pragma once

#include "src/managed_app_focus.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace managed_app_focus {
  // One poll of the focus loop; apply({}, allowed) only observes. A game window
  // that already has the foreground spends no attempt. The clock is read after
  // the blocking observation, so a delayed reply cannot admit an expired window.
  // Adapters ask allowed(identity) immediately before each activation, so a
  // second blocking observation cannot carry it past the allowance either.
  // Only the preferred window is activated: a launcher that takes focus
  // instead is not a confirmed game focus.
  template<class Apply, class Now>
  bool focus_step(Apply &&apply, budget &focus, Now &&clock_now) {
    const std::function<bool(const std::string &)> still_allowed = [&](const std::string &identity) {
      return focus.can_focus(identity, clock_now());
    };
    const nlohmann::json observed = apply(std::vector<std::string> {}, still_allowed);
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
    // Adapters list windows in a strict preference order that does not depend
    // on focus. A fullscreen game window ahead of the target keeps the
    // foreground even after its allowance ended, so a newer dialog does not
    // take it. Rank by tiling or age alone is a heuristic and never holds: any
    // other window in front, even one of the game's own process, is lost focus.
    const auto foreground = observed.contains("foreground") && observed["foreground"].is_string() ? observed["foreground"].get<std::string>() : std::string {};
    const bool foreground_fullscreen = observed.contains("foreground_fullscreen") && observed["foreground_fullscreen"] == true;
    if (allowed.empty() || foreground == allowed.front()) {
      return false;
    }
    for (const auto &window : observed["windows"]) {
      if (!window.is_string() || window.get<std::string>() == allowed.front()) {
        break;
      }
      if (foreground_fullscreen && window.get<std::string>() == foreground) {
        return false;
      }
    }
    const nlohmann::json applied = apply(std::vector<std::string> {allowed.front()}, still_allowed);
    if (!applied.is_object() || !applied.contains("focused") || applied["focused"] != true) {
      return false;
    }
    focus.confirmed();
    return true;
  }
}  // namespace managed_app_focus

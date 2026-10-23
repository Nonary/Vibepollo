#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <cassert>
#include <charconv>
#include <iostream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  // A compositor adapter fake with the shared KWin/Hyprland result shape.
  struct fake_desktop {
    std::vector<std::string> windows {"701:0x5a02", "700:0x5a01"};  // Fullscreen game first.
    std::uint64_t foreground = 0;
    int activations = 0;

    nlohmann::json operator()(const std::vector<std::string> &activate) {
      if (!activate.empty()) {
        ++activations;
        const auto &identity = activate.front();
        foreground = 0;
        std::from_chars(identity.data(), identity.data() + identity.size(), foreground);
      }
      return {{"windows", windows}, {"focused", !activate.empty()}, {"foreground_pid", foreground}};
    }
  };
}  // namespace

int main() {
  using managed_app_focus::budget;
  using managed_app_focus::focus_step;
  const std::vector<std::uint64_t> pids {700, 701};
  const auto start = budget::clock::time_point(1h);
  {
    // A game that keeps the foreground spends no attempts, so a later steal
    // within the window is still recovered.
    fake_desktop desktop;
    budget focus({3, 15, false}, start);
    desktop.foreground = 900;
    assert(focus_step(desktop, focus, pids, start));
    assert(desktop.activations == 1 && desktop.foreground == 701);
    for (int second = 1; second < 6; ++second) {
      assert(!focus_step(desktop, focus, pids, start + std::chrono::seconds(second)));
    }
    assert(desktop.activations == 1);
    desktop.foreground = 900;  // Big Picture or a browser takes focus.
    assert(focus_step(desktop, focus, pids, start + 6s));
    assert(desktop.activations == 2 && focus.active(start + 6s));
  }
  {
    // The game's own launcher in front does not count as the game.
    fake_desktop desktop;
    budget focus({3, 15, false}, start);
    desktop.foreground = 700;
    assert(focus_step(desktop, focus, pids, start));
    assert(desktop.foreground == 701);
  }
  std::cout << "PASS: managed focus spends attempts only when the game lost the foreground\n";
  return 0;
}

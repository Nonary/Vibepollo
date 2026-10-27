#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  // A compositor adapter fake with the shared KWin/Hyprland result shape.
  struct fake_desktop {
    std::vector<std::string> windows {"701:0x5a02", "700:0x5a01"};  // Fullscreen game first.
    std::string foreground;
    int activations = 0;

    nlohmann::json operator()(const std::vector<std::string> &activate) {
      if (!activate.empty()) {
        ++activations;
        foreground = activate.front();
      }
      return {{"windows", windows}, {"focused", !activate.empty()}, {"foreground", foreground}};
    }
  };
}  // namespace

int main() {
  using managed_app_focus::budget;
  using managed_app_focus::focus_step;
  const auto start = budget::clock::time_point(1h);
  const auto at = [](budget::clock::time_point now) {
    return [now] { return now; };
  };
  {
    // A game that keeps the foreground spends no attempts, so a later steal
    // within the window is still recovered.
    fake_desktop desktop;
    budget focus({3, 15, false}, start);
    desktop.foreground = "900:0x5a03";
    assert(focus_step(desktop, focus, at(start)));
    assert(desktop.activations == 1 && desktop.foreground == "701:0x5a02");
    for (int second = 1; second < 6; ++second) {
      assert(!focus_step(desktop, focus, at(start + std::chrono::seconds(second))));
    }
    assert(desktop.activations == 1);
    desktop.foreground = "900:0x5a03";  // Big Picture or a browser takes focus.
    assert(focus_step(desktop, focus, at(start + 6s)));
    assert(desktop.activations == 2 && focus.active(start + 6s));
  }
  {
    // The game's own launcher in front does not count as the game.
    fake_desktop desktop;
    budget focus({3, 15, false}, start);
    desktop.foreground = "700:0x5a01";
    assert(focus_step(desktop, focus, at(start)));
    assert(desktop.foreground == "701:0x5a02");
  }
  {
    // Nor does a launcher or dialog of the game's own process.
    fake_desktop desktop;
    desktop.windows = {"700:0xaaaa", "700:0xbbbb"};
    desktop.foreground = "700:0xbbbb";
    budget focus({1, 15, false}, start);
    assert(focus_step(desktop, focus, at(start)));
    assert(desktop.activations == 1 && desktop.foreground == "700:0xaaaa");
  }
  {
    // The exact game window in front needs no activation and spends nothing.
    fake_desktop desktop;
    desktop.foreground = "701:0x5a02";
    budget focus({1, 15, false}, start);
    assert(!focus_step(desktop, focus, at(start)));
    assert(desktop.activations == 0 && focus.can_focus(start));
  }
  {
    // An expired fullscreen window still listed first is not the game; the
    // newer eligible window in front spends nothing, and a later loss of it
    // is still recovered.
    fake_desktop desktop;
    desktop.windows = {"701:0x5a02"};
    budget focus({1, 15, false}, start);
    desktop.foreground = "701:0x5a02";
    assert(!focus_step(desktop, focus, at(start)));
    desktop.windows = {"701:0x5a02", "702:0x5a03"};
    assert(!focus_step(desktop, focus, at(start + 10s)));
    desktop.foreground = "702:0x5a03";
    for (int second = 16; second < 19; ++second) {
      assert(!focus_step(desktop, focus, at(start + std::chrono::seconds(second))));
    }
    assert(desktop.activations == 0);
    desktop.foreground = "900:0x5a04";
    assert(focus_step(desktop, focus, at(start + 19s)));
    assert(desktop.activations == 1 && desktop.foreground == "702:0x5a03");
  }
  {
    // A compositor reply delayed past a window's allowance does not let the
    // expired window be activated.
    fake_desktop desktop;
    desktop.windows = {"701:0x5a02"};
    budget focus({1, 15, false}, start);
    desktop.foreground = "701:0x5a02";
    assert(!focus_step(desktop, focus, at(start)));
    desktop.foreground = "900:0x5a04";
    auto now = start + 14s;
    assert(!focus_step([&](const std::vector<std::string> &activate) {
      now = start + 16s;  // The observation blocks across the deadline.
      return desktop(activate);
    }, focus, [&] { return now; }));
    assert(desktop.activations == 0);
  }
  std::cout << "PASS: managed focus spends attempts only when the game lost the foreground\n";
  return 0;
}

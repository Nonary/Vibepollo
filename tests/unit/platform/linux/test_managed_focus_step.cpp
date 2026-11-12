#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  // A compositor adapter fake with the shared KWin/Hyprland result shape.
  struct fake_desktop {
    std::vector<std::string> windows {"701:0x5a02", "700:0x5a01"};  // Fullscreen game first.
    std::string foreground;
    bool foreground_fullscreen = false;
    int activations = 0;
    std::function<void()> before_activation;  // The adapter's second observation.

    nlohmann::json operator()(const std::vector<std::string> &activate, const std::function<bool(const std::string &)> &allowed) {
      if (!activate.empty() && before_activation) {
        before_activation();
      }
      const bool focused = !activate.empty() && allowed(activate.front());
      if (focused) {
        ++activations;
        foreground = activate.front();
      }
      return {{"windows", windows}, {"focused", focused}, {"foreground", foreground}, {"foreground_fullscreen", foreground_fullscreen}};
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
    // A fullscreen game window whose allowance ended keeps the foreground it
    // has: a lower-ranked dialog that appeared later does not take it, and the
    // game losing focus to another program still sends focus to the dialog.
    fake_desktop desktop;
    desktop.windows = {"701:0x5a02"};
    budget focus({3, 15, false}, start);
    desktop.foreground = "701:0x5a02";
    desktop.foreground_fullscreen = true;
    assert(!focus_step(desktop, focus, at(start)));
    desktop.windows = {"701:0x5a02", "701:0x5a09"};
    for (int second = 10; second < 20; ++second) {
      assert(!focus_step(desktop, focus, at(start + std::chrono::seconds(second))));
    }
    assert(desktop.activations == 0 && focus.can_focus(start + 20s));
    desktop.foreground = "900:0x5a04";
    desktop.foreground_fullscreen = false;
    assert(focus_step(desktop, focus, at(start + 20s)));
    assert(desktop.activations == 1 && desktop.foreground == "701:0x5a09");
  }
  {
    // A fullscreen program that takes the foreground is lost focus, and so is
    // a fullscreen game window ranked below the target.
    fake_desktop desktop;
    budget focus({3, 15, false}, start);
    desktop.foreground = "900:0x5a03";
    desktop.foreground_fullscreen = true;
    assert(focus_step(desktop, focus, at(start)));
    assert(desktop.activations == 1 && desktop.foreground == "701:0x5a02");
    desktop.foreground = "700:0x5a01";
    desktop.foreground_fullscreen = true;
    assert(focus_step(desktop, focus, at(start + 1s)));
    assert(desktop.activations == 2 && desktop.foreground == "701:0x5a02");
  }
  {
    // Ranking by tiling or age is a guess and never holds: a tiled launcher
    // whose allowance ended stays lost focus for a game window that maps
    // later, even one ranked below it.
    fake_desktop desktop;
    desktop.windows = {"700:0x5a01"};
    budget focus({3, 15, false}, start);
    desktop.foreground = "700:0x5a01";
    assert(!focus_step(desktop, focus, at(start)));
    desktop.windows = {"700:0x5a01", "701:0x5a02"};
    assert(focus_step(desktop, focus, at(start + 30s)));
    assert(desktop.activations == 1 && desktop.foreground == "701:0x5a02");
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
    assert(!focus_step([&](const std::vector<std::string> &activate, const auto &allowed) {
      now = start + 16s;  // The observation blocks across the deadline.
      return desktop(activate, allowed);
    }, focus, [&] { return now; }));
    assert(desktop.activations == 0);
  }
  {
    // Nor does a second observation by the adapter that blocks across the
    // deadline, before it would dispatch.
    fake_desktop desktop;
    desktop.windows = {"701:0x5a02"};
    budget focus({1, 15, false}, start);
    desktop.foreground = "701:0x5a02";
    assert(!focus_step(desktop, focus, at(start)));
    desktop.foreground = "900:0x5a04";
    auto now = start + 14s + 900ms;
    desktop.before_activation = [&] { now = start + 15s + 200ms; };
    assert(!focus_step(desktop, focus, [&] { return now; }));
    assert(desktop.activations == 0);
  }
  {
    // Adapter data of the wrong type is lost focus, never an exception, and
    // only the preferred window is handed to the adapter.
    budget focus({1, 15, false}, start);
    std::vector<std::string> handed;
    const auto odd = [&](const std::vector<std::string> &activate, const auto &) {
      if (!activate.empty()) handed = activate;
      return nlohmann::json {{"windows", {"701:0x5a02", "700:0x5a01"}}, {"focused", activate.empty() ? nlohmann::json(false) : nlohmann::json("yes")}, {"foreground", 1}};
    };
    assert(!focus_step(odd, focus, at(start)));
    assert((handed == std::vector<std::string> {"701:0x5a02"}));
    assert(focus.can_focus(start));
  }
  std::cout << "PASS: managed focus spends attempts only when the game lost the foreground\n";
  return 0;
}

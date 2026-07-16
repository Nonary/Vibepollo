#ifdef _WIN32

#include "src/platform/windows/game_activity.h"

#include <gtest/gtest.h>

#include <array>

namespace {
  using platf::game_activity::signal_source_e;
  using platf::game_activity::signal_t;

  TEST(GameActivity, HighestPriorityPositiveSignalWins) {
    const std::array signals {
      signal_t {signal_source_e::fullscreen_foreground, true, 10, "fallback.exe"},
      signal_t {signal_source_e::tracked_process, true, 20, "tracked.exe"},
      signal_t {signal_source_e::playnite, true, 30, "playnite-game.exe"},
    };

    const auto state = platf::game_activity::reduce_signals(signals);

    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.source, signal_source_e::playnite);
    EXPECT_EQ(state.pid, 30u);
    EXPECT_EQ(state.executable, "playnite-game.exe");
  }

  TEST(GameActivity, RetractingOneSourceLeavesOtherClaimsActive) {
    const std::array signals {
      signal_t {signal_source_e::playnite, false, 0, {}},
      signal_t {signal_source_e::tracked_process, true, 20, "tracked.exe"},
      signal_t {signal_source_e::fullscreen_foreground, true, 10, "fallback.exe"},
    };

    const auto state = platf::game_activity::reduce_signals(signals);

    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.source, signal_source_e::tracked_process);
    EXPECT_EQ(state.executable, "tracked.exe");
  }

}

#endif

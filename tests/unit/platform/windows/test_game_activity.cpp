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

  TEST(GameActivity, VisibleFullscreenGameWinsOverDesktopBehindIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .belongs_to_active_app = true,
        .fullscreen_on_capture_display = true,
      },
      visible_window_evidence_t {
        .desktop_ui = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_TRUE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, ActiveAppOwnedOverlayAllowsFullscreenGameBelowIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .belongs_to_active_app = true,
      },
      visible_window_evidence_t {
        .belongs_to_active_app = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_TRUE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, VisibleDesktopUiBlocksFullscreenGameBelowIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .desktop_ui = true,
      },
      visible_window_evidence_t {
        .belongs_to_active_app = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_FALSE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, WindowedGameDoesNotHideDesktopBehindIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .belongs_to_active_app = true,
      },
      visible_window_evidence_t {
        .desktop_ui = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_FALSE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, UnrelatedVisibleWindowBlocksFullscreenGameBelowIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {},
      visible_window_evidence_t {
        .belongs_to_active_app = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_FALSE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, PassiveCompositorHostAllowsFullscreenGameBelowIt) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .passive_host = true,
        .fullscreen_on_capture_display = true,
      },
      visible_window_evidence_t {
        .belongs_to_active_app = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_TRUE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      true
    ));
  }

  TEST(GameActivity, NoActivateLayeredToolStyleIsPassive) {
    constexpr std::uintptr_t borderless_visible_popup = 0x94000000;
    constexpr std::uintptr_t layered_noactivate_tool = 0x08080080;

    EXPECT_TRUE(platf::foreground_app::passive_compositor_style_for_tests(
      borderless_visible_popup,
      layered_noactivate_tool
    ));
  }

  TEST(GameActivity, NormalActivatableWindowStyleIsNotPassive) {
    constexpr std::uintptr_t normal_window = 0x14C70000;
    constexpr std::uintptr_t window_edge = 0x00000100;

    EXPECT_FALSE(platf::foreground_app::passive_compositor_style_for_tests(
      normal_window,
      window_edge
    ));
  }

  TEST(GameActivity, DesktopPlaceboSkipsPassiveHostBeforeFullscreenApplication) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .passive_host = true,
        .fullscreen_on_capture_display = true,
      },
      visible_window_evidence_t {
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_TRUE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      false
    ));
  }

  TEST(GameActivity, DesktopPlaceboRejectsFullscreenShellSurface) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .desktop_ui = true,
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_FALSE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      false
    ));
  }

  TEST(GameActivity, DesktopPlaceboAcceptsTopmostFullscreenApplication) {
    using platf::foreground_app::visible_window_evidence_t;
    const std::array evidence {
      visible_window_evidence_t {
        .fullscreen_on_capture_display = true,
      },
    };

    EXPECT_TRUE(platf::foreground_app::visible_fullscreen_game_selected_for_tests(
      evidence,
      false
    ));
  }

  TEST(GameActivity, OwnDisplayTransitionPreservesKnownGameResize) {
    platf::foreground_app::state_t sample;
    sample.source = "desktop-visible";
    sample.matching_window_seen = true;

    platf::foreground_app::state_t last_confirmed;
    last_confirmed.fullscreen_on_capture_display = true;

    EXPECT_TRUE(platf::game_activity::preserve_confirmed_game_during_display_transition(
      sample,
      last_confirmed,
      true
    ));
  }

  TEST(GameActivity, OwnDisplayTransitionPreservesIndeterminateComposition) {
    platf::foreground_app::state_t sample;
    sample.source = "visibility-unknown";

    platf::foreground_app::state_t last_confirmed;
    last_confirmed.fullscreen_on_capture_display = true;

    EXPECT_TRUE(platf::game_activity::preserve_confirmed_game_during_display_transition(
      sample,
      last_confirmed,
      true
    ));
  }

  TEST(GameActivity, ConfirmedDesktopStillDemotesDuringDisplayTransition) {
    platf::foreground_app::state_t sample;
    sample.source = "desktop-visible";
    sample.matching_window_seen = false;

    platf::foreground_app::state_t last_confirmed;
    last_confirmed.fullscreen_on_capture_display = true;

    EXPECT_FALSE(platf::game_activity::preserve_confirmed_game_during_display_transition(
      sample,
      last_confirmed,
      true
    ));
  }

  TEST(GameActivity, OwnDisplayTransitionMinimumHoldPreventsImmediateModeReversal) {
    platf::foreground_app::state_t sample;
    sample.source = "desktop-visible";

    platf::foreground_app::state_t last_confirmed;
    last_confirmed.fullscreen_on_capture_display = true;

    EXPECT_TRUE(platf::game_activity::preserve_confirmed_game_during_display_transition(
      sample,
      last_confirmed,
      true,
      true
    ));
  }

}

#endif

/**
 * @file tests/unit/test_playnite_focus_policy.cpp
 * @brief Tests for the Playnite launcher autofocus deadline policy.
 */
#include <gtest/gtest.h>
#include <tools/playnite_launcher/focus_policy.h>

namespace {
  using namespace std::chrono_literals;
  using playnite_launcher::focus::policy::clock;
  using playnite_launcher::focus::policy::focus_deadline;
  using playnite_launcher::focus::policy::k_window_wait_limit;

  const clock::time_point k_armed {std::chrono::hours(1)};

  TEST(PlayniteFocusPolicy, WaitsForFirstWindowBeyondFocusTimeout) {
    EXPECT_EQ(focus_deadline(k_armed, std::nullopt, 15s), k_armed + k_window_wait_limit);
  }

  TEST(PlayniteFocusPolicy, FocusTimeoutRunsFromLateWindow) {
    // Regression: a game window appearing 16s after gameStarted used to miss a
    // 15s focus budget that had been counting from process start.
    EXPECT_EQ(focus_deadline(k_armed, k_armed + 16s, 15s), k_armed + 31s);
  }

  TEST(PlayniteFocusPolicy, EarlyWindowKeepsTimeoutFromArming) {
    EXPECT_EQ(focus_deadline(k_armed, k_armed - 5s, 15s), k_armed + 15s);
  }

  TEST(PlayniteFocusPolicy, NewWindowsCannotExtendPastWaitLimit) {
    EXPECT_EQ(focus_deadline(k_armed, k_armed + k_window_wait_limit, 15s), k_armed + k_window_wait_limit);
  }

  TEST(PlayniteFocusPolicy, LongConfiguredTimeoutIsNotClampedByWaitLimit) {
    const auto timeout = k_window_wait_limit + 60s;
    EXPECT_EQ(focus_deadline(k_armed, std::nullopt, timeout), k_armed + timeout);
    EXPECT_EQ(focus_deadline(k_armed, k_armed, timeout), k_armed + timeout);
  }
}  // namespace

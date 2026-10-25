/**
 * @file tests/unit/platform/macos/test_display_policy.cpp
 * @brief Tests for the macOS lid, exclusive layout, and capture recovery decisions.
 */
#include "../../../tests_common.h"

#include <src/platform/macos/display_policy.h>

using namespace platf::macos_display_policy;
using namespace std::literals;

TEST(MacosLidClosePolicy, QuitsTheAppAndEndsRemoteStreamsWithIt) {
  // A game, a Remote Monitor, and a Remote Input stream: the app's exit ends only the game's.
  const auto plan = plan_lid_close(true, {stream_role_e::app, stream_role_e::remote, stream_role_e::remote}, true);
  EXPECT_TRUE(plan.quit_app);
  EXPECT_EQ(plan.end_streams, (std::vector<std::size_t> {1, 2}));
}

TEST(MacosLidClosePolicy, EndsRemoteOnlyStreams) {
  const auto plan = plan_lid_close(true, {stream_role_e::remote, stream_role_e::remote}, false);
  EXPECT_FALSE(plan.quit_app);
  EXPECT_EQ(plan.end_streams, (std::vector<std::size_t> {0, 1}));
}

TEST(MacosLidClosePolicy, RemoteStreamsEndEvenWhileAnAppRunsWithoutItsStream) {
  const auto plan = plan_lid_close(true, {stream_role_e::remote}, true);
  EXPECT_TRUE(plan.quit_app);
  EXPECT_EQ(plan.end_streams, (std::vector<std::size_t> {0}));
}

TEST(MacosLidClosePolicy, LeavesTheAppsStreamsToItsExit) {
  const auto plan = plan_lid_close(true, {stream_role_e::app}, true);
  EXPECT_TRUE(plan.quit_app);
  EXPECT_TRUE(plan.end_streams.empty());
}

TEST(MacosLidClosePolicy, EndsStreamsWhenNoAppRuns) {
  const auto plan = plan_lid_close(true, {stream_role_e::app, stream_role_e::app}, false);
  EXPECT_FALSE(plan.quit_app);
  EXPECT_EQ(plan.end_streams, (std::vector<std::size_t> {0, 1}));
}

TEST(MacosLidClosePolicy, DoesNothingWithoutStreams) {
  for (const bool app_running : {true, false}) {
    const auto plan = plan_lid_close(true, {}, app_running);
    EXPECT_FALSE(plan.quit_app);
    EXPECT_TRUE(plan.end_streams.empty());
  }
}

TEST(MacosLidClosePolicy, KeepsStreamingWhenTheSettingIsOff) {
  for (const bool app_running : {true, false}) {
    const auto plan = plan_lid_close(false, {stream_role_e::app, stream_role_e::remote}, app_running);
    EXPECT_FALSE(plan.quit_app);
    EXPECT_TRUE(plan.end_streams.empty());
  }
}

TEST(MacosExclusiveLayoutPolicy, NothingToDoWhenOnlyTheStreamsDisplaysAreOn) {
  const auto plan = plan_exclusive_layout({5}, {5}, {1});
  EXPECT_TRUE(plan.turn_off.empty());
  EXPECT_TRUE(plan.newly_tracked.empty());
}

TEST(MacosExclusiveLayoutPolicy, TurnsOffTheBuiltInDisplayWhenTheLidReopens) {
  // The built-in display (1) was turned off when the stream started and came back with the lid.
  const auto plan = plan_exclusive_layout({1, 5}, {5}, {1});
  EXPECT_EQ(plan.turn_off, (std::vector<display_id_t> {1}));
  EXPECT_TRUE(plan.newly_tracked.empty());
}

TEST(MacosExclusiveLayoutPolicy, TracksDisplaysThatAppearDuringTheSession) {
  // Started with the lid closed, so nothing was turned off; then the lid opened and a monitor was plugged in.
  const auto plan = plan_exclusive_layout({1, 5, 9}, {5}, {});
  EXPECT_EQ(plan.turn_off, (std::vector<display_id_t> {1, 9}));
  EXPECT_EQ(plan.newly_tracked, (std::vector<display_id_t> {1, 9}));
}

TEST(MacosExclusiveLayoutPolicy, LeavesRemoteMonitorsOn) {
  const auto plan = plan_exclusive_layout({1, 5, 7, 8}, {5, 7, 8}, {1});
  EXPECT_EQ(plan.turn_off, (std::vector<display_id_t> {1}));
}

TEST(MacosExclusiveLayoutPolicy, LeavesNewDisplaysAloneWhileARemoteMonitorIsBeingMade) {
  // 7 is a Remote Monitor that's online but not yet published; 1 is the built-in display, already
  // turned off once and back with the lid.
  const auto plan = plan_exclusive_layout({1, 5, 7}, {5}, {1}, true);
  EXPECT_EQ(plan.turn_off, (std::vector<display_id_t> {1}));
  EXPECT_TRUE(plan.newly_tracked.empty());
}

TEST(MacosExclusiveLayoutPolicy, InitialLayoutWaitsForARemoteMonitorBeingMade) {
  // The game's display (5) comes up while a Remote Monitor (7) is still being made.
  const auto pending = plan_exclusive_layout({1, 5, 7}, {5}, {}, true);
  EXPECT_TRUE(pending.turn_off.empty());
  // Once it's published, only the physical display is turned off.
  const auto published = plan_exclusive_layout({1, 5, 7}, {5, 7}, {}, false);
  EXPECT_EQ(published.turn_off, (std::vector<display_id_t> {1}));
}

TEST(MacosExclusiveLayoutPolicy, ModeReplacementKeepsTheRemoteMonitorOn) {
  // The old Remote Monitor display (7) is gone and its replacement (8) is still being made.
  const auto plan = plan_exclusive_layout({5, 8}, {5}, {1}, true);
  EXPECT_TRUE(plan.turn_off.empty());
}

TEST(MacosExclusiveLayoutPolicy, IgnoresDuplicateEntries) {
  const auto plan = plan_exclusive_layout({1, 1, 5}, {5}, {});
  EXPECT_EQ(plan.turn_off, (std::vector<display_id_t> {1}));
  EXPECT_EQ(plan.newly_tracked, (std::vector<display_id_t> {1}));
}

TEST(MacosAttemptBudget, StopsAfterTheLimitWithinTheWindow) {
  attempt_budget_t budget {3, 60s};
  const auto start = clock::time_point {};
  EXPECT_TRUE(budget.try_spend(start));
  EXPECT_TRUE(budget.try_spend(start + 1s));
  EXPECT_TRUE(budget.try_spend(start + 2s));
  EXPECT_FALSE(budget.try_spend(start + 3s));
  EXPECT_FALSE(budget.try_spend(start + 59s));
}

TEST(MacosAttemptBudget, RecoversAsOldAttemptsAge) {
  attempt_budget_t budget {2, 10s};
  const auto start = clock::time_point {};
  EXPECT_TRUE(budget.try_spend(start));
  EXPECT_TRUE(budget.try_spend(start + 5s));
  EXPECT_FALSE(budget.try_spend(start + 9s));
  EXPECT_TRUE(budget.try_spend(start + 10s));
  EXPECT_FALSE(budget.try_spend(start + 11s));
  EXPECT_TRUE(budget.try_spend(start + 15s));
}

TEST(MacosAttemptBudget, ResetForgetsAttempts) {
  attempt_budget_t budget {1, 60s};
  const auto start = clock::time_point {};
  EXPECT_TRUE(budget.try_spend(start));
  EXPECT_FALSE(budget.try_spend(start + 1s));
  budget.reset();
  EXPECT_TRUE(budget.try_spend(start + 2s));
}

TEST(MacosDisplayPresence, BriefDropoutsDontCount) {
  display_presence_t presence {2s};
  const auto start = clock::time_point {};
  EXPECT_FALSE(presence.lost(false, start));
  EXPECT_FALSE(presence.lost(false, start + 1s));
  EXPECT_FALSE(presence.lost(true, start + 1500ms));
  EXPECT_FALSE(presence.lost(false, start + 2500ms));
  EXPECT_FALSE(presence.lost(false, start + 4s));
}

TEST(MacosDisplayPresence, LostAfterTheGracePeriod) {
  display_presence_t presence {2s};
  const auto start = clock::time_point {};
  EXPECT_FALSE(presence.lost(false, start));
  EXPECT_TRUE(presence.lost(false, start + 2s));
  EXPECT_TRUE(presence.lost(false, start + 3s));
  EXPECT_FALSE(presence.lost(true, start + 4s));
}

namespace {
  /// Records what cover_and_turn_off() does, and fails where a test asks it to.
  struct fake_safeguards_t {
    bool marker_write_fails = false;
    bool helper_fails = false;
    bool turn_off_fails = false;

    std::optional<std::vector<display_id_t>> marker;
    std::vector<std::vector<display_id_t>> helpers_started;
    std::vector<std::pair<int, bool>> helpers_stopped;  ///< pid, and whether "done" was sent.
    std::vector<std::vector<display_id_t>> turned_off;
    int next_pid = 100;

    safeguard_ops_t ops() {
      return {
        .write_marker = [this](const auto &ids) {
          if (marker_write_fails) {
            marker = std::vector<display_id_t> {0xBAD};  // partly written
            return false;
          }
          marker = ids;
          return true;
        },
        .remove_marker = [this]() {
          marker.reset();
        },
        .start_helper = [this](const auto &ids) {
          if (helper_fails) {
            return restore_helper_t {};
          }
          helpers_started.push_back(ids);
          return restore_helper_t {next_pid++, 3};
        },
        .stop_helper = [this](restore_helper_t &helper, const bool restored) {
          helpers_stopped.emplace_back(helper.pid, restored);
          helper = {};
        },
        .turn_off = [this](const auto &ids) {
          if (turn_off_fails) {
            return false;
          }
          turned_off.push_back(ids);
          return true;
        },
      };
    }
  };
}  // namespace

TEST(MacosSafeguards, InitialSetupCoversBeforeTurningOff) {
  fake_safeguards_t fake;
  restore_helper_t helper;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {}, {1, 2}, {1, 2}), cover_result_e::turned_off);
  EXPECT_EQ(fake.marker, (std::vector<display_id_t> {1, 2}));
  EXPECT_EQ(fake.helpers_started, (std::vector<std::vector<display_id_t>> {{1, 2}}));
  EXPECT_EQ(fake.turned_off, (std::vector<std::vector<display_id_t>> {{1, 2}}));
  EXPECT_TRUE(helper.running());
  EXPECT_TRUE(fake.helpers_stopped.empty());
}

TEST(MacosSafeguards, InitialSetupTurnsNothingOffWhenTheMarkerCantBeWritten) {
  fake_safeguards_t fake;
  fake.marker_write_fails = true;
  restore_helper_t helper;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {}, {1, 2}, {1, 2}), cover_result_e::safeguards_failed);
  EXPECT_TRUE(fake.turned_off.empty());
  EXPECT_TRUE(fake.helpers_started.empty());
  EXPECT_FALSE(fake.marker);
  EXPECT_FALSE(helper.running());
}

TEST(MacosSafeguards, InitialSetupTurnsNothingOffWhenTheHelperCantStart) {
  // Covers both a failed pipe and a failed spawn: either way start_watchdog() returns no helper.
  fake_safeguards_t fake;
  fake.helper_fails = true;
  restore_helper_t helper;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {}, {1, 2}, {1, 2}), cover_result_e::safeguards_failed);
  EXPECT_TRUE(fake.turned_off.empty());
  EXPECT_FALSE(fake.marker);
  EXPECT_FALSE(helper.running());
}

TEST(MacosSafeguards, InitialSetupUndoesTheSafeguardsWhenTurningOffFails) {
  fake_safeguards_t fake;
  fake.turn_off_fails = true;
  restore_helper_t helper;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {}, {1, 2}, {1, 2}), cover_result_e::turn_off_failed);
  EXPECT_FALSE(fake.marker);
  EXPECT_EQ(fake.helpers_stopped, (std::vector<std::pair<int, bool>> {{100, true}}));
  EXPECT_FALSE(helper.running());
}

TEST(MacosSafeguards, HotplugReplacesTheHelperOnlyOnceTheDisplayIsOff) {
  fake_safeguards_t fake;
  restore_helper_t helper {42, 7};
  fake.marker = std::vector<display_id_t> {1};
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {1}, {1, 9}, {9}), cover_result_e::turned_off);
  EXPECT_EQ(fake.marker, (std::vector<display_id_t> {1, 9}));
  EXPECT_EQ(fake.turned_off, (std::vector<std::vector<display_id_t>> {{9}}));
  EXPECT_EQ(helper.pid, 100);
  EXPECT_EQ(fake.helpers_stopped, (std::vector<std::pair<int, bool>> {{42, true}}));
}

TEST(MacosSafeguards, HotplugKeepsTheCurrentCoverageWhenTheMarkerCantBeWritten) {
  fake_safeguards_t fake;
  restore_helper_t helper {42, 7};
  fake.marker = std::vector<display_id_t> {1};
  fake.marker_write_fails = true;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {1}, {1, 9}, {9}), cover_result_e::safeguards_failed);
  EXPECT_TRUE(fake.turned_off.empty());
  EXPECT_TRUE(fake.helpers_stopped.empty());
  EXPECT_EQ(helper.pid, 42);
  EXPECT_TRUE(helper.running());
}

TEST(MacosSafeguards, HotplugKeepsTheCurrentHelperWhenTheNewOneCantStart) {
  fake_safeguards_t fake;
  restore_helper_t helper {42, 7};
  fake.marker = std::vector<display_id_t> {1};
  fake.helper_fails = true;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {1}, {1, 9}, {9}), cover_result_e::safeguards_failed);
  EXPECT_TRUE(fake.turned_off.empty());
  EXPECT_TRUE(fake.helpers_stopped.empty());
  EXPECT_EQ(helper.pid, 42);
  EXPECT_EQ(fake.marker, (std::vector<display_id_t> {1}));
}

TEST(MacosSafeguards, HotplugKeepsTheCurrentHelperWhenTurningOffFails) {
  fake_safeguards_t fake;
  restore_helper_t helper {42, 7};
  fake.marker = std::vector<display_id_t> {1};
  fake.turn_off_fails = true;
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {1}, {1, 9}, {9}), cover_result_e::turn_off_failed);
  EXPECT_EQ(helper.pid, 42);
  EXPECT_EQ(fake.marker, (std::vector<display_id_t> {1}));
  EXPECT_EQ(fake.helpers_stopped, (std::vector<std::pair<int, bool>> {{100, true}}));
}

TEST(MacosSafeguards, RestartsAMissingHelperBeforeTurningCoveredDisplaysOffAgain) {
  // The built-in display (1) came back with the lid, but the helper covering it isn't running.
  fake_safeguards_t fake;
  restore_helper_t helper;
  fake.marker = std::vector<display_id_t> {1};
  EXPECT_EQ(cover_and_turn_off(fake.ops(), helper, {1}, {1}, {1}), cover_result_e::turned_off);
  EXPECT_EQ(fake.helpers_started, (std::vector<std::vector<display_id_t>> {{1}}));
  EXPECT_TRUE(helper.running());
}

TEST(MacosVirtualDisplaySize, RetinaIsHalfThePixelsInPoints) {
  const auto size = virtual_display_size(3840, 2160, true);
  EXPECT_EQ(size.width, 1920);
  EXPECT_EQ(size.height, 1080);
  EXPECT_EQ(size.pixel_width, 3840);
  EXPECT_EQ(size.pixel_height, 2160);
}

TEST(MacosVirtualDisplaySize, StandardDensityIsOnePointPerPixel) {
  const auto size = virtual_display_size(1920, 1080, false);
  EXPECT_EQ(size.width, 1920);
  EXPECT_EQ(size.pixel_width, 1920);
}

TEST(MacosVirtualDisplaySize, OddRetinaSizesRoundDown) {
  const auto size = virtual_display_size(2161, 1081, true);
  EXPECT_EQ(size.width, 1080);
  EXPECT_EQ(size.pixel_width, 2160);
  EXPECT_EQ(size.pixel_height, 1080);
}

TEST(MacosPointerDisplay, ParsesOnlyWholeDisplayIds) {
  EXPECT_EQ(parse_display_identity("7"), std::optional<display_id_t> {7});
  EXPECT_FALSE(parse_display_identity(""));
  EXPECT_FALSE(parse_display_identity("0"));
  EXPECT_FALSE(parse_display_identity("7a"));
  EXPECT_FALSE(parse_display_identity("HDMI-A-1"));
}

TEST(MacosPointerDisplay, RemoteStartupThenANormalPrimaryDisplay) {
  // A Remote Monitor (7) started at (0, 0) with no game running; then a game made its virtual
  // display (5) the main display at (0, 0), moving 7 aside. Each stream's input stays on its own display.
  const auto is_remote = [](const display_id_t id) {
    return id == 7;
  };
  EXPECT_EQ(pointer_display(display_id_t {7}, is_remote, 5), 7u);
  EXPECT_EQ(pointer_display(display_id_t {5}, is_remote, 5), 5u);
}

TEST(MacosPointerDisplay, NormalStreamsFollowTheStreamedDisplay) {
  const auto is_remote = [](const display_id_t id) {
    return id == 7;
  };
  // Captured before the game's virtual display existed, or with no identity at all.
  EXPECT_EQ(pointer_display(display_id_t {1}, is_remote, 5), 5u);
  EXPECT_EQ(pointer_display(std::nullopt, is_remote, 5), 5u);
}

TEST(MacosPointerDisplay, AGoneRemoteMonitorFallsBackToTheStreamedDisplay) {
  const auto none = [](display_id_t) {
    return false;
  };
  EXPECT_EQ(pointer_display(display_id_t {7}, none, 1), 1u);
}

TEST(MacosPointerDisplay, TwoRemoteMonitorsStayApart) {
  // After a rearrangement, either display may sit where the other used to.
  const auto is_remote = [](const display_id_t id) {
    return id == 7 || id == 8;
  };
  EXPECT_EQ(pointer_display(display_id_t {7}, is_remote, 1), 7u);
  EXPECT_EQ(pointer_display(display_id_t {8}, is_remote, 1), 8u);
}

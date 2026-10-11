/**
 * @file tests/unit/platform/macos/test_display_policy.cpp
 * @brief Tests for the macOS lid, exclusive layout, and capture recovery decisions.
 */
#include "../../../tests_common.h"

#include <src/platform/macos/display_policy.h>

using namespace platf::macos_display_policy;
using namespace std::literals;

TEST(MacosLidClosePolicy, EndsStreamsByDefaultBehavior) {
  EXPECT_EQ(lid_close_action(true, 1, true), lid_close_action_e::quit_app);
  EXPECT_EQ(lid_close_action(true, 2, false), lid_close_action_e::end_streams);
}

TEST(MacosLidClosePolicy, DoesNothingWithoutStreams) {
  EXPECT_EQ(lid_close_action(true, 0, true), lid_close_action_e::none);
  EXPECT_EQ(lid_close_action(true, 0, false), lid_close_action_e::none);
}

TEST(MacosLidClosePolicy, KeepsStreamingWhenTheSettingIsOff) {
  EXPECT_EQ(lid_close_action(false, 1, true), lid_close_action_e::none);
  EXPECT_EQ(lid_close_action(false, 3, false), lid_close_action_e::none);
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

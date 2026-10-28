#include "src/managed_app_focus.h"
#include "src/managed_app_focus_candidates.h"

#include <gtest/gtest.h>

TEST(ManagedAppFocus, LegacyAliasesAndExplicitSharedPrecedence) {
  std::unordered_map<std::string, std::string> vars {
    {"playnite_focus_attempts", "7"},
    {"app_focus_attempts", "0"},
    {"playnite_focus_timeout_secs", "40"},
    {"playnite_focus_exit_on_first", "yes"},
    {"app_focus_exit_on_first", "off"},
    {"playnite_enabled", "true"}
  };
  const auto options = managed_app_focus::parse_policy(vars);
  EXPECT_EQ(options.attempts, 0);
  EXPECT_EQ(options.timeout_secs, 40);
  EXPECT_FALSE(options.exit_on_first);
  EXPECT_FALSE(options.enabled());
  ASSERT_EQ(vars.size(), 1U);
  EXPECT_TRUE(vars.contains("playnite_enabled"));
}

TEST(ManagedAppFocus, InvalidAndBoundedValues) {
  std::unordered_map<std::string, std::string> vars {
    {"app_focus_attempts", "12junk"},
    {"app_focus_timeout_secs", "9999"}
  };
  auto options = managed_app_focus::parse_policy(vars);
  EXPECT_EQ(options.attempts, 3);
  EXPECT_EQ(options.timeout_secs, 300);
  vars = {{"app_focus_attempts", "-1"}, {"app_focus_timeout_secs", "0"}};
  options = managed_app_focus::parse_policy(vars);
  EXPECT_EQ(options.attempts, 0);
  EXPECT_FALSE(options.enabled());
}

TEST(ManagedAppFocus, DiscoveryFailuresDoNotSpendConfirmedBudget) {
  using namespace managed_app_focus;
  const auto start = budget::clock::time_point {};
  budget retry({3, 15, false}, start);
  EXPECT_TRUE(retry.active(start + std::chrono::seconds(14)));
  retry.confirmed();
  retry.confirmed();
  EXPECT_TRUE(retry.active(start + std::chrono::seconds(14)));
  retry.confirmed();
  EXPECT_FALSE(retry.active(start + std::chrono::seconds(14)));
}

TEST(ManagedAppFocus, FirstSuccessAndHardDiscoveryLimitStopFocus) {
  using namespace managed_app_focus;
  const auto start = budget::clock::time_point {};
  budget first({3, 15, true}, start);
  first.confirmed();
  EXPECT_FALSE(first.active(start));
  budget timeout({3, 15, false}, start);
  EXPECT_TRUE(timeout.active(start + std::chrono::seconds(25)));
  EXPECT_FALSE(timeout.active(start + std::chrono::seconds(180)));
  budget disabled({0, 15, false}, start);
  EXPECT_FALSE(disabled.active(start));
}

TEST(ManagedAppFocus, DisabledPolicyIsPassedExplicitlyToPlaynite) {
  EXPECT_EQ(managed_app_focus::launcher_arguments({0, 0, false}), " --focus-attempts 0 --focus-timeout 0");
  EXPECT_EQ(managed_app_focus::launcher_arguments({3, 15, true}), " --focus-attempts 3 --focus-timeout 15 --focus-exit-on-first");
}

TEST(ManagedAppFocus, SteamIdentitySelectsProtonAndExcludesOtherGamesAndSteam) {
  platf::steam::lifecycle::process_snapshot snapshot;
  snapshot.processes[1] = {.pid = 1, .executable = "/usr/bin/wine", .steam_app_id = 42};
  snapshot.processes[2] = {.pid = 2, .executable = "/games/game/game", .steam_app_id = 43};
  snapshot.processes[3] = {.pid = 3, .executable = "/usr/bin/steam", .steam_app_id = 42};
  EXPECT_EQ(managed_app_focus::candidates({"steam", "42", "/games/game"}, snapshot), (std::vector<std::uint64_t> {1}));
}

TEST(ManagedAppFocus, LutrisAcceptsGameWorkingDirectoryButRejectsOtherAppsAndBroadPaths) {
  platf::steam::lifecycle::process_snapshot snapshot;
  snapshot.processes[1] = {.pid = 1, .executable = "/usr/bin/wine", .cwd = "/games/game"};
  snapshot.processes[2] = {.pid = 2, .executable = "/games/game-other/game", .cwd = "/games/game-other"};
  EXPECT_EQ(managed_app_focus::candidates({"lutris", "42", "/games/game"}, snapshot), (std::vector<std::uint64_t> {1}));
  EXPECT_TRUE(managed_app_focus::candidates({"lutris", "42", "/"}, snapshot).empty());
  EXPECT_TRUE(managed_app_focus::candidates({"unknown", "42", "/games/game"}, snapshot).empty());
}

TEST(ManagedAppFocus, LateFirstWindowGetsConfiguredFocusTime) {
  using namespace managed_app_focus;
  using namespace std::chrono_literals;
  const auto start = budget::clock::time_point {};
  for (auto late : {16s, 25s}) {
    budget retry({3, 15, false}, start);
    EXPECT_TRUE(retry.active(start + late));
    EXPECT_FALSE(retry.can_focus(start + late));
    EXPECT_TRUE(retry.observe("7:game-window", start + late));
    EXPECT_TRUE(retry.can_focus(start + late + 14s));
    EXPECT_FALSE(retry.can_focus(start + late + 15s));
    EXPECT_TRUE(retry.active(start + late + 15s));
  }
}

TEST(ManagedAppFocus, ReplacementWindowRearmsButRepeatedObservationDoesNot) {
  using namespace managed_app_focus;
  using namespace std::chrono_literals;
  const auto start = budget::clock::time_point {};
  budget retry({3, 15, false}, start);
  retry.observe("7:splash", start + 1s);
  retry.confirmed();
  EXPECT_FALSE(retry.observe("7:splash", start + 16s));
  EXPECT_FALSE(retry.can_focus(start + 16s));
  EXPECT_TRUE(retry.observe("7:game", start + 25s));
  EXPECT_EQ(retry.deadline(), start + 40s);
  EXPECT_FALSE(retry.can_focus("7:splash", start + 25s));
  EXPECT_TRUE(retry.can_focus("7:game", start + 25s));
  retry.refresh(start + 26s);
  EXPECT_FALSE(retry.can_focus("7:splash", start + 26s));
  EXPECT_TRUE(retry.can_focus("7:game", start + 40s));
  EXPECT_TRUE(retry.observe("8:game", start + 39s));
  EXPECT_EQ(retry.deadline(), start + 54s);
  retry.confirmed();
  EXPECT_TRUE(retry.active(start + 39s));
  retry.confirmed();
  EXPECT_FALSE(retry.active(start + 39s));
  EXPECT_FALSE(retry.observe("9:new", start + 40s));
}

TEST(ManagedAppFocus, HardCapBoundsNewWindowsAndStatusRefresh) {
  using namespace managed_app_focus;
  using namespace std::chrono_literals;
  const auto start = budget::clock::time_point {};
  budget retry({100, 15, false}, start);
  for (int n = 0; n < 180; ++n) {
    retry.observe(std::to_string(n), start + std::chrono::seconds(n));
    retry.refresh(start + std::chrono::seconds(n));
    EXPECT_LE(retry.deadline(), start + 180s);
  }
  EXPECT_FALSE(retry.active(start + 180s));
  EXPECT_FALSE(retry.can_focus(start + 180s));
  EXPECT_FALSE(retry.observe("late", start + 180s));
}

TEST(ManagedAppFocus, TimeoutLongerThanDiscoveryFloorIsPreserved) {
  using namespace managed_app_focus;
  using namespace std::chrono_literals;
  const auto start = budget::clock::time_point {};
  budget retry({3, 240, false}, start);
  EXPECT_TRUE(retry.active(start + 239s));
  retry.observe("7:game", start + 25s);
  EXPECT_EQ(retry.deadline(), start + 240s);
  EXPECT_TRUE(retry.can_focus(start + 239s));
  EXPECT_FALSE(retry.active(start + 240s));
}

TEST(ManagedAppFocus, DisabledPolicyCannotArmWindows) {
  using namespace managed_app_focus;
  const auto start = budget::clock::time_point {};
  for (auto options : {policy {0,15,false}, policy {3,0,false}}) {
    budget disabled(options, start);
    EXPECT_FALSE(disabled.active(start));
    EXPECT_FALSE(disabled.observe("7:game", start));
    EXPECT_FALSE(disabled.can_focus(start));
  }
}

TEST(ManagedAppFocus, EmptyIdentityDoesNotArmAndNewSessionForgetsOldWindows) {
  using namespace managed_app_focus;
  using namespace std::chrono_literals;
  const auto start = budget::clock::time_point {};
  budget first({3, 15, false}, start);
  EXPECT_FALSE(first.observe("", start));
  EXPECT_FALSE(first.can_focus(start));
  EXPECT_TRUE(first.observe("7:game", start + 25s));
  budget next({3, 15, false}, start + 26s);
  EXPECT_TRUE(next.observe("7:game", start + 26s));
  EXPECT_EQ(next.deadline(), start + 41s);
}

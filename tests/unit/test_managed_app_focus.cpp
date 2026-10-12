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

TEST(ManagedAppFocus, FirstSuccessAndDeadlineStopFocus) {
  using namespace managed_app_focus;
  const auto start = budget::clock::time_point {};
  budget first({3, 15, true}, start);
  first.confirmed();
  EXPECT_FALSE(first.active(start));
  budget timeout({3, 15, false}, start);
  EXPECT_FALSE(timeout.active(start + std::chrono::seconds(15)));
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

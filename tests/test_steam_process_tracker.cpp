#include "src/steam_process_tracker.h"
#include "src/steam_big_picture_policy.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace lifecycle = platf::steam::lifecycle;

TEST(SteamTrackingExit, PersistsUntilCleanupEvenWhenTheLauncherSurvives) {
  lifecycle::exit_latch state;
  lifecycle::association_result gone;
  gone.reason = "no game processes found";
  state.observe(true, gone);
  EXPECT_TRUE(state.exited);
  // A retry after lifecycle-gate contention must still bypass launcher
  // auto-detach, regardless of whether the game exited before or after
  // the startup deadline.
  state.observe(false, gone);
  EXPECT_TRUE(state.exited);
  state = {};
  EXPECT_FALSE(state.exited);
}

TEST(SteamTrackingExit, DoesNotEndUnassociatedOrTemporarilyUnavailableGames) {
  lifecycle::exit_latch state;
  lifecycle::association_result result;
  result.reason = "no game processes found";
  state.observe(false, result);
  EXPECT_FALSE(state.exited);
  result.reason = "process snapshot unavailable";
  state.observe(true, result);
  EXPECT_FALSE(state.exited);
  // tracker::finish retains association for an incomplete snapshot.
  result.outcome = lifecycle::association_outcome::associated;
  state.observe(true, result);
  EXPECT_FALSE(state.exited);
}

namespace {

  lifecycle::process_info process(std::uint64_t pid, std::uint64_t parent,
                                  const char *exe, const char *cwd = "",
                                  std::vector<std::string> command_line = {}) {
    lifecycle::process_info result;
    result.pid = pid;
    result.parent_pid = parent;
    result.executable = exe;
    result.cwd = cwd;
    result.command_line = std::move(command_line);
    return result;
  }

  lifecycle::process_snapshot snapshot(std::initializer_list<lifecycle::process_info> entries) {
    lifecycle::process_snapshot result;
    for (const auto &entry : entries) {
      result.processes.emplace(entry.pid, entry);
    }
    return result;
  }

  lifecycle::process_info steam_process(std::uint64_t pid, std::uint64_t parent,
                                       const char *exe, std::uint32_t app_id) {
    auto result = process(pid, parent, exe);
    result.steam_app_id = app_id;
    result.start_time_ticks = pid * 100;
    return result;
  }

  class fake_controller final : public lifecycle::process_controller {
  public:
    std::map<lifecycle::process_id_t, bool> living;
    std::vector<std::pair<lifecycle::process_id_t, lifecycle::signal_kind>> signals;
    bool identity_ok = true;
    bool exits_on_term = false;

    bool signal(lifecycle::process_id_t pid, lifecycle::signal_kind kind) override {
      signals.emplace_back(pid, kind);
      if (kind == lifecycle::signal_kind::kill || exits_on_term) {
        living[pid] = false;
      }
      return true;
    }
    bool alive(lifecycle::process_id_t pid) override { return living[pid]; }
    bool identity_matches(lifecycle::process_id_t, const lifecycle::process_info &) override {
      return identity_ok;
    }
    void sleep_for(std::chrono::milliseconds) override {
      // A fake process ignores TERM, causing the bounded KILL path to run
      // without making the unit test wait for real time.
    }
  };

}  // namespace

TEST(SteamBigPicture, ExistingGamesAndTheirLaterChildrenAreExcluded) {
  const auto before = snapshot({steam_process(10, 1, "/games/old/game", 41), process(2, 1, "/usr/bin/steam")});
  const auto after = snapshot({steam_process(10, 1, "/games/old/game", 41),
                              steam_process(11, 10, "/games/old/renderer", 41),
                              steam_process(20, 2, "/games/new/game", 42),
                              steam_process(21, 20, "/usr/bin/steamwebhelper", 42),
                              steam_process(22, 20, "/outside/renderer", 42),
                              process(30, 1, "/unrelated/game")});
  const auto tree = lifecycle::big_picture_tree(before, after);
  ASSERT_EQ(tree.processes.size(), 2U);
  EXPECT_TRUE(tree.processes.contains(20));
  EXPECT_TRUE(tree.processes.contains(22));
}

TEST(SteamBigPicture, IncompleteBaselineDisablesCleanup) {
  auto before = snapshot({});
  before.complete = false;
  EXPECT_TRUE(lifecycle::big_picture_tree(before, snapshot({steam_process(20, 1, "/game", 42)})).empty());
}

TEST(SteamBigPicture, ParsesOnlyCompleteNumericSteamAppIdEnvironmentEntries) {
  using namespace std::literals;
  EXPECT_EQ(lifecycle::big_picture_app_id("OTHER=42\0SteamAppId=123\0SECRET=ignored\0"sv), 123U);
  EXPECT_EQ(lifecycle::big_picture_app_id("SteamAppId=0\0"sv), 0U);
  EXPECT_EQ(lifecycle::big_picture_app_id("SteamAppId=123"sv), 0U);
  EXPECT_EQ(lifecycle::big_picture_app_id("SteamAppId=123junk\0"sv), 0U);
  EXPECT_EQ(lifecycle::big_picture_app_id("SteamAppId=-1\0"sv), 0U);
  EXPECT_EQ(lifecycle::big_picture_app_id("SteamAppId=4294967296\0"sv), 0U);
  EXPECT_EQ(lifecycle::big_picture_app_id("NotSteamAppId=123\0"sv), 0U);
}

TEST(SteamBigPicture, MultipleGamesShareOneGracePeriodAndProtectSteam) {
  const auto before = snapshot({process(2, 1, "/usr/bin/steam")});
  const auto after = snapshot({process(2, 1, "/usr/bin/steam"),
                              steam_process(20, 2, "/games/one/game", 42),
                              steam_process(30, 2, "/games/two/game", 43)});
  const auto tree = lifecycle::big_picture_tree(before, after);
  fake_controller controller;
  controller.living = {{2, true}, {20, true}, {30, true}};
  const auto result = lifecycle::stop_tree(tree, controller);
  EXPECT_EQ(result.terminate_sent, 2U);
  EXPECT_EQ(result.kill_sent, 2U);
  EXPECT_TRUE(controller.living[2]);
  EXPECT_TRUE(result.complete);
}

TEST(SteamBigPicture, BaselinePidReuseAndExitedGamesAreNotClaimed) {
  const auto before = snapshot({process(20, 1, "/unrelated/program")});
  const auto after = snapshot({steam_process(20, 1, "/games/new/game", 42)});
  EXPECT_TRUE(lifecycle::big_picture_tree(before, after).empty());
  EXPECT_TRUE(lifecycle::big_picture_tree(before, snapshot({})).empty());
}

TEST(SteamBigPicture, CooperativeGameExitsWithoutForceKill) {
  const auto tree = lifecycle::big_picture_tree(snapshot({}), snapshot({steam_process(20, 1, "/game", 42)}));
  fake_controller controller;
  controller.living = {{20, true}};
  controller.exits_on_term = true;
  const auto result = lifecycle::stop_tree(tree, controller);
  EXPECT_EQ(result.terminate_sent, 1U);
  EXPECT_EQ(result.kill_sent, 0U);
  EXPECT_TRUE(result.complete);
}

TEST(SteamBigPicture, MissingProcessIdentityDisablesSignalling) {
  auto game = steam_process(20, 1, "/game", 42);
  game.start_time_ticks = 0;
  EXPECT_TRUE(lifecycle::big_picture_tree(snapshot({}), snapshot({game})).empty());
}

TEST(SteamBigPicture, SharedToolsDoNotClaimExistingGameOrUntaggedProcesses) {
  const auto before = snapshot({steam_process(10, 1, "/games/old/game", 41)});
  const auto after = snapshot({steam_process(20, 1, "/tools/proton/wine", 41),
                              steam_process(30, 1, "/tools/proton/wine", 42),
                              process(40, 1, "/tools/proton/updater")});
  const auto tree = lifecycle::big_picture_tree(before, after);
  ASSERT_EQ(tree.processes.size(), 1U);
  EXPECT_TRUE(tree.processes.contains(30));
}

TEST(SteamProcessTracker, AssociatesOnlyNewInstallProcesses) {
  const auto root = std::filesystem::path("/tmp/steam/library/steamapps/common/Game");
  const auto old_game = process(10, 1, "/tmp/steam/library/steamapps/common/Game/game", root.string().c_str());
  const auto baseline = snapshot({old_game, process(1, 0, "/sbin/init")});
  const auto after = snapshot({old_game, process(1, 0, "/sbin/init"),
                               process(20, 1, "/tmp/steam/library/steamapps/common/Game/game", root.string().c_str())});

  const auto result = lifecycle::associate(baseline, after, root);
  ASSERT_TRUE(result.associated());
  ASSERT_EQ(result.tree.processes.size(), 1U);
  EXPECT_TRUE(result.tree.processes.contains(20));
  EXPECT_FALSE(result.tree.processes.contains(10));
}

TEST(SteamProcessTracker, PathBoundaryDoesNotMatchSiblingDirectory) {
  const auto baseline = lifecycle::process_snapshot {};
  const auto after = snapshot({process(20, 1, "/tmp/steam/library/steamapps/common/Gamepad/game",
                                       "/tmp/steam/library/steamapps/common/Gamepad")});
  const auto result = lifecycle::associate(baseline, after,
                                           "/tmp/steam/library/steamapps/common/Game");
  EXPECT_EQ(result.outcome, lifecycle::association_outcome::untrackable);
  EXPECT_FALSE(lifecycle::path_is_within("/tmp/SteamGame", "/tmp/Steam"));
}

TEST(SteamProcessTracker, ProtectsSteamClientAndExplicitRoot) {
  const auto root = std::filesystem::path("/games/Example");
  const auto steam = process(30, 1, "/usr/lib/steam/steam", root.string().c_str(), {"steam"});
  const auto wrapper = process(31, 1, "/opt/launcher", root.string().c_str());
  const auto baseline = lifecycle::process_snapshot {};
  const auto after = snapshot({steam, wrapper});
  lifecycle::association_options options;
  options.protected_steam_roots = {31};
  const auto result = lifecycle::associate(baseline, after, root, options);
  EXPECT_EQ(result.outcome, lifecycle::association_outcome::untrackable);
  EXPECT_TRUE(lifecycle::is_protected_steam_process(steam));
  EXPECT_TRUE(lifecycle::is_protected_steam_process(wrapper, options));
}

TEST(SteamProcessTracker, ExpandsNewDescendantsOutsideInstallDirectory) {
  const auto root = std::filesystem::path("/games/Example");
  const auto baseline = snapshot({process(1, 0, "/sbin/init")});
  const auto after = snapshot({process(1, 0, "/sbin/init"),
                               process(40, 1, "/games/Example/game", root.string().c_str()),
                               process(41, 40, "/usr/bin/renderer", "/tmp"),
                               process(42, 41, "/usr/bin/child", "/tmp")});
  const auto result = lifecycle::associate(baseline, after, root);
  ASSERT_TRUE(result.associated());
  EXPECT_EQ(result.tree.root_pid, 40U);
  EXPECT_EQ(result.tree.processes.size(), 3U);
  EXPECT_TRUE(result.tree.processes.contains(41));
  EXPECT_TRUE(result.tree.processes.contains(42));
}

TEST(SteamProcessTracker, ProtonCommandLineIsAssociationEvidence) {
  const auto root = std::filesystem::path("/games/Example");
  const auto proton = process(50, 1, "/usr/bin/proton", "/tmp",
                             {"proton", "/games/Example/Game.exe"});
  const auto baseline = lifecycle::process_snapshot {};
  const auto result = lifecycle::associate(baseline, snapshot({proton}), root);
  ASSERT_TRUE(result.associated());
  EXPECT_TRUE(result.tree.processes.contains(50));
}

TEST(SteamProcessTracker, ReportsBaselineOnlyAndUntrackableSeparately) {
  const auto root = std::filesystem::path("/games/Example");
  const auto existing = process(60, 1, "/games/Example/game", root.string().c_str());
  const auto baseline = snapshot({existing});
  EXPECT_EQ(lifecycle::associate(baseline, baseline, root).outcome,
            lifecycle::association_outcome::baseline_only);
  const auto unrelated = snapshot({existing, process(61, 1, "/usr/bin/editor", "/tmp")});
  EXPECT_EQ(lifecycle::associate(baseline, unrelated, root).outcome,
            lifecycle::association_outcome::untrackable);
}

TEST(SteamProcessTracker, UsesAvailableRecordsFromAnIncompleteProcSnapshot) {
  const auto root = std::filesystem::path("/games/Example");
  const auto baseline = lifecycle::process_snapshot {};
  auto after = snapshot({process(62, 1, "/games/Example/game", root.string().c_str())});
  after.complete = false;  // another /proc entry disappeared during enumeration
  const auto result = lifecycle::associate(baseline, after, root);
  EXPECT_EQ(result.outcome, lifecycle::association_outcome::associated);
  EXPECT_TRUE(result.tree.processes.contains(62));
}

TEST(SteamProcessTracker, GracefullyStopsDescendantsThenKillsOnlyTrackedTree) {
  lifecycle::tracked_tree tree;
  tree.processes.emplace(70, lifecycle::tracked_process {process(70, 1, "/games/game"), false});
  tree.processes.emplace(71, lifecycle::tracked_process {process(71, 70, "/games/child"), false});
  tree.processes.emplace(72, lifecycle::tracked_process {process(72, 1, "/usr/bin/steam"), true});
  fake_controller controller;
  controller.living = {{70, true}, {71, true}, {72, true}, {999, true}};
  lifecycle::stop_options options;
  options.grace_period = std::chrono::milliseconds(0);
  const auto result = lifecycle::stop_tree(tree, controller, options);
  EXPECT_EQ(result.terminate_sent, 2U);
  EXPECT_EQ(result.kill_sent, 2U);
  EXPECT_EQ(result.skipped, 1U);
  ASSERT_EQ(controller.signals.size(), 4U);
  EXPECT_EQ(controller.signals[0].first, 71U);
  EXPECT_EQ(controller.signals[1].first, 70U);
  EXPECT_EQ(controller.signals[2].second, lifecycle::signal_kind::kill);
  EXPECT_EQ(controller.signals[3].second, lifecycle::signal_kind::kill);
  EXPECT_TRUE(controller.living[72]);
  EXPECT_TRUE(controller.living[999]);
}

TEST(SteamProcessTracker, RefusesToSignalWhenPidIdentityChanged) {
  lifecycle::tracked_tree tree;
  tree.processes.emplace(80, lifecycle::tracked_process {process(80, 1, "/games/game"), false});
  fake_controller controller;
  controller.living = {{80, true}};
  controller.identity_ok = false;
  const auto result = lifecycle::stop_tree(tree, controller);
  EXPECT_EQ(result.terminate_sent, 0U);
  EXPECT_EQ(result.kill_sent, 0U);
  EXPECT_EQ(result.skipped, 1U);
  EXPECT_TRUE(controller.signals.empty());
}

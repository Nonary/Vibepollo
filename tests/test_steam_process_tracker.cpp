#include "src/steam_process_tracker.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace lifecycle = platf::steam::lifecycle;

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

  class fake_controller final : public lifecycle::process_controller {
  public:
    std::map<lifecycle::process_id_t, bool> living;
    std::vector<std::pair<lifecycle::process_id_t, lifecycle::signal_kind>> signals;
    bool identity_ok = true;

    bool signal(lifecycle::process_id_t pid, lifecycle::signal_kind kind) override {
      signals.emplace_back(pid, kind);
      if (kind == lifecycle::signal_kind::kill) {
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

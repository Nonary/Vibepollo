/**
 * @file tests/unit/test_builtin_virtual_display.cpp
 * @brief Regression coverage for global hooks on the generated desktop entry.
 */
#include "../tests_common.h"

#include <mutex>
#include <src/builtin_virtual_display.h>
#include <src/prep_command_lifetime.h>
#include <thread>

namespace {
  auto identity = [](const std::string &value) {
    return value;
  };

  TEST(BuiltinVirtualDisplay, EmptyGlobalsPreserveDesktopLifetimeAndPermissions) {
    const auto app =
      proc::make_builtin_virtual_display(config::sunshine_t {}, identity);
    EXPECT_EQ(app.uuid, VIRTUAL_DISPLAY_UUID);
    EXPECT_EQ(app.name, "Virtual Display");
    EXPECT_EQ(app.image_path, "virtual_desktop.png");
    EXPECT_TRUE(app.virtual_display);
    EXPECT_EQ(app.scale_factor, 100);
    EXPECT_TRUE(app.auto_detach);
    EXPECT_EQ(app.exit_timeout, std::chrono::seconds(5));
    EXPECT_FALSE(app.terminate_on_pause);
    EXPECT_FALSE(app.allow_client_commands);
    EXPECT_FALSE(app.elevated);
    EXPECT_FALSE(app.wait_all);
    EXPECT_FALSE(app.use_app_identity);
    EXPECT_FALSE(app.per_client_app_identity);
    EXPECT_TRUE(app.cmd.empty());
    EXPECT_TRUE(app.detached.empty());
    EXPECT_TRUE(app.prep_cmds.empty());
    EXPECT_TRUE(app.state_cmds.empty());
  }

  TEST(BuiltinVirtualDisplay, InheritsBothHookListsInOrderWithExpansionAndElevation) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("$(HOOK) prepare one", "$(HOOK) undo one", true);
    global.prep_cmds.emplace_back("$(HOOK) prepare two", "$(HOOK) undo two", false);
    global.state_cmds.emplace_back("$(HOOK) resume", "$(HOOK) pause", true);
    std::vector<std::string> expanded;
    const auto app =
      proc::make_builtin_virtual_display(global, [&](std::string value) {
        expanded.push_back(value);
        if (value.starts_with("$(HOOK)")) {
          value.replace(0, 7, "notification script");
        }
        return value;
      });

    ASSERT_EQ(app.prep_cmds.size(), 2u);
    EXPECT_EQ(app.prep_cmds[0].do_cmd, "notification script prepare one");
    EXPECT_EQ(app.prep_cmds[0].undo_cmd, "notification script undo one");
    EXPECT_TRUE(app.prep_cmds[0].elevated);
    EXPECT_EQ(app.prep_cmds[1].do_cmd, "notification script prepare two");
    EXPECT_EQ(app.prep_cmds[1].undo_cmd, "notification script undo two");
    EXPECT_FALSE(app.prep_cmds[1].elevated);
    ASSERT_EQ(app.state_cmds.size(), 1u);
    EXPECT_EQ(app.state_cmds[0].do_cmd, "notification script resume");
    EXPECT_EQ(app.state_cmds[0].undo_cmd, "notification script pause");
    EXPECT_TRUE(app.state_cmds[0].elevated);
    // Expansion is delegated to the same parse_env_val callback used by
    // apps.json.
    EXPECT_EQ(expanded.size(), 7u);
    EXPECT_EQ(global.prep_cmds[0].do_cmd, "$(HOOK) prepare one");
    EXPECT_EQ(global.prep_cmds[0].undo_cmd, "$(HOOK) undo one");
    EXPECT_TRUE(global.prep_cmds[0].elevated);
  }

  TEST(BuiltinVirtualDisplay, KeepsUndoOnlyAndResumeOnlyHooks) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("", "undo only", false);
    global.state_cmds.emplace_back("resume only", "", false);
    const auto app = proc::make_builtin_virtual_display(global, identity);
    ASSERT_EQ(app.prep_cmds.size(), 1u);
    EXPECT_TRUE(app.prep_cmds[0].do_cmd.empty());
    EXPECT_EQ(app.prep_cmds[0].undo_cmd, "undo only");
    ASSERT_EQ(app.state_cmds.size(), 1u);
    EXPECT_EQ(app.state_cmds[0].do_cmd, "resume only");
    EXPECT_TRUE(app.state_cmds[0].undo_cmd.empty());
  }

  TEST(BuiltinVirtualDisplay, ReparseAndActiveAppSnapshotsDoNotConsumeOrDuplicateGlobals) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("prepare", "undo", true);
    global.state_cmds.emplace_back("resume", "pause", false);
    auto first = proc::make_builtin_virtual_display(global, identity);
    // proc_t copies one app snapshot at execute(); all clients share that app.
    const auto active = first;
    first.prep_cmds.clear();
    first.state_cmds.clear();
    for (int parse = 0; parse < 3; ++parse) {
      const auto refreshed = proc::make_builtin_virtual_display(global, identity);
      ASSERT_EQ(refreshed.prep_cmds.size(), 1u);
      ASSERT_EQ(refreshed.state_cmds.size(), 1u);
      EXPECT_EQ(refreshed.prep_cmds[0].do_cmd, "prepare");
      EXPECT_EQ(refreshed.state_cmds[0].undo_cmd, "pause");
    }
    ASSERT_EQ(active.prep_cmds.size(), 1u);
    ASSERT_EQ(active.state_cmds.size(), 1u);
    EXPECT_EQ(active.prep_cmds[0].undo_cmd, "undo");
    EXPECT_EQ(active.state_cmds[0].do_cmd, "resume");
    EXPECT_TRUE(global.prep_cmds[0].elevated);
  }

  TEST(BuiltinVirtualDisplay, FailedPreparationRollsBackOnlyCompletedPrefixOnce) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("prepare one", "undo one", false);
    global.prep_cmds.emplace_back("prepare failing", "undo failing", false);
    global.prep_cmds.emplace_back("prepare unreached", "undo unreached", false);
    const auto app = proc::make_builtin_virtual_display(global, identity);
    auto next = app.prep_cmds.cbegin();
    std::vector<std::string> ran;
    EXPECT_FALSE(proc::command_lifetime::prepare(next, app.prep_cmds.cend(), [&](const auto &cmd) {
      ran.push_back(cmd.do_cmd);
      return cmd.do_cmd != "prepare failing";
    }));
    const auto undo = [&](const auto &cmd) {
      ran.push_back(cmd.undo_cmd);
    };
    proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), undo);
    proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), undo);
    EXPECT_EQ(ran, (std::vector<std::string> {"prepare one", "prepare failing", "undo one"}));
  }

  TEST(BuiltinVirtualDisplay, FirstPreparationFailureHasNothingToUndo) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("fails", "must not undo", false);
    const auto app = proc::make_builtin_virtual_display(global, identity);
    auto next = app.prep_cmds.cbegin();
    EXPECT_FALSE(proc::command_lifetime::prepare(next, app.prep_cmds.cend(), [](const auto &) {
      return false;
    }));
    proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), [](const auto &) {
      FAIL() << "No prep completed";
    });
  }

  TEST(BuiltinVirtualDisplay, LaunchFailureAfterPreparationAndTerminationUndoInReverseOrder) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("first", "undo first", false);
    global.prep_cmds.emplace_back("", "undo only", false);
    global.prep_cmds.emplace_back("last", "undo last", false);
    const auto app = proc::make_builtin_virtual_display(global, identity);
    auto next = app.prep_cmds.cbegin();
    std::vector<std::string> ran;
    ASSERT_TRUE(proc::command_lifetime::prepare(next, app.prep_cmds.cend(), [&](const auto &cmd) {
      ran.push_back(cmd.do_cmd);
      return true;
    }));
    // The app owns one completed prefix while clients join, leave and resume.
    // No preparation runs per client. Failed app launch and final termination
    // both consume this same prefix, with repeated cleanup becoming a no-op.
    const auto undo = [&](const auto &cmd) {
      ran.push_back(cmd.undo_cmd);
    };
    proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), undo);
    proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), undo);
    EXPECT_EQ(ran, (std::vector<std::string> {"first", "last", "undo last", "undo only", "undo first"}));
  }

  TEST(BuiltinVirtualDisplay, ConcurrentClientsShareOnePreparationCursorUnderLifecycleGate) {
    config::sunshine_t global {};
    global.prep_cmds.emplace_back("prepare", "undo", false);
    const auto app = proc::make_builtin_virtual_display(global, identity);
    auto next = app.prep_cmds.cbegin();
    std::mutex lifecycle_gate;
    std::vector<std::string> ran;
    // Launch/resume admission and cleanup hold the transport lifecycle gate.
    // The cursor belongs to the shared app and is not reset for another client.
    const auto attach = [&] {
      std::lock_guard lock(lifecycle_gate);
      EXPECT_TRUE(proc::command_lifetime::prepare(next, app.prep_cmds.cend(), [&](const auto &cmd) {
        ran.push_back(cmd.do_cmd);
        return true;
      }));
    };
    std::thread first(attach), second(attach);
    first.join();
    second.join();
    EXPECT_EQ(ran, (std::vector<std::string> {"prepare"}));
    const auto cleanup = [&] {
      std::lock_guard lock(lifecycle_gate);
      proc::command_lifetime::undo(next, app.prep_cmds.cbegin(), [&](const auto &cmd) {
        ran.push_back(cmd.undo_cmd);
      });
    };
    std::thread first_cleanup(cleanup), second_cleanup(cleanup);
    first_cleanup.join();
    second_cleanup.join();
    EXPECT_EQ(ran, (std::vector<std::string> {"prepare", "undo"}));
  }
}  // namespace

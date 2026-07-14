/**
 * @file tests/unit/test_foreground_suspend.cpp
 */
#include "../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/foreground_suspend.h>

namespace {
  namespace foreground = platf::foreground_suspend;

  foreground::identity_t valid_identity(std::string basename = "game.exe") {
    foreground::identity_t identity;
    identity.pid = 4242;
    identity.process_path = "C:\\Games\\Example\\" + basename;
    identity.executable_basename = std::move(basename);
    identity.creation_time = 123456;
    identity.session_id = 1;
    identity.active_session_id = 1;
    identity.owner_sid = "S-1-5-21-1000";
    identity.active_user_sid = "S-1-5-21-1000";
    identity.system_root = "C:\\Windows";
    identity.critical = false;
    identity.protected_process = false;
    return identity;
  }

  TEST(ForegroundSuspendPolicy, MatchesLaunchersCaseInsensitivelyAcrossSupportedFamilies) {
    const std::vector<std::string> launchers {
      "STEAM.EXE",
      "steamwebhelper.exe",
      "Playnite.DesktopApp.exe",
      "playnite.fullscreenapp.exe",
      "playnite-launcher.exe",
      "Battle.net.exe",
      "Agent.exe",
      "EADesktop.exe",
      "EABackgroundService.exe",
      "UbisoftConnect.exe",
      "upc.exe",
      "XboxPcApp.exe",
      "GamingServices.exe",
      "GameBar.exe",
      "EpicGamesLauncher.exe",
      "EpicWebHelper.exe",
      "GalaxyClient.exe",
      "GalaxyClientService.exe",
      "RiotClientServices.exe",
      "RiotClientUxRender.exe",
      "AmazonGames.exe",
      "Amazon Games UI.exe",
    };
    for (const auto &launcher : launchers) {
      EXPECT_TRUE(foreground::is_known_launcher_basename(launcher)) << launcher;
      auto identity = valid_identity(launcher);
      const auto result = foreground::evaluate_policy(identity);
      EXPECT_FALSE(result.allowed) << launcher;
      EXPECT_EQ(result.reason, foreground::rejection_reason_e::known_launcher) << launcher;
    }
  }

  TEST(ForegroundSuspendPolicy, RejectsWindowsPathsAndShellUiProcesses) {
    auto system_path = valid_identity("notepad.exe");
    system_path.process_path = "c:/WINDOWS/System32/notepad.exe";
    EXPECT_EQ(foreground::evaluate_policy(system_path).reason, foreground::rejection_reason_e::system_path);

    for (const auto *name : {"RuntimeBroker.exe", "ShellExperienceHost.exe", "StartMenuExperienceHost.exe",
                             "SearchHost.exe", "TextInputHost.exe", "ApplicationFrameHost.exe", "SystemSettings.exe"}) {
      auto identity = valid_identity(name);
      EXPECT_EQ(foreground::evaluate_policy(identity).reason, foreground::rejection_reason_e::system_process) << name;
    }
  }

  TEST(ForegroundSuspendPolicy, RejectsCriticalProtectedCrossSessionAndCrossUserTargets) {
    auto critical = valid_identity();
    critical.critical = true;
    EXPECT_EQ(foreground::evaluate_policy(critical).reason, foreground::rejection_reason_e::critical_process);

    auto protected_process = valid_identity();
    protected_process.protected_process = true;
    EXPECT_EQ(foreground::evaluate_policy(protected_process).reason, foreground::rejection_reason_e::protected_process);

    auto wrong_session = valid_identity();
    wrong_session.session_id = 2;
    EXPECT_EQ(foreground::evaluate_policy(wrong_session).reason, foreground::rejection_reason_e::wrong_session);

    auto wrong_user = valid_identity();
    wrong_user.owner_sid = "S-1-5-21-2000";
    EXPECT_EQ(foreground::evaluate_policy(wrong_user).reason, foreground::rejection_reason_e::wrong_user);
  }

  TEST(ForegroundSuspendPolicy, RejectsMissingIdentity) {
    auto missing_path = valid_identity();
    missing_path.process_path.clear();
    EXPECT_EQ(foreground::evaluate_policy(missing_path).reason, foreground::rejection_reason_e::missing_identity);

    auto missing_owner = valid_identity();
    missing_owner.owner_sid.clear();
    EXPECT_EQ(foreground::evaluate_policy(missing_owner).reason, foreground::rejection_reason_e::missing_identity);

    auto missing_critical_query = valid_identity();
    missing_critical_query.critical.reset();
    EXPECT_EQ(foreground::evaluate_policy(missing_critical_query).reason, foreground::rejection_reason_e::missing_identity);
  }

  TEST(ForegroundSuspendPolicy, AcceptsGameAndOrdinaryDiscordForegroundProcesses) {
    EXPECT_TRUE(foreground::evaluate_policy(valid_identity("game.exe")).allowed);
    // This intentionally documents the accepted best-effort behavior: an ordinary
    // foreground app such as Discord may be suspended if the user leaves it focused.
    EXPECT_TRUE(foreground::evaluate_policy(valid_identity("Discord.exe")).allowed);
  }

  TEST(ForegroundSuspendSelection, ChoosesOwnedGroupForDirectNonLauncher) {
    EXPECT_EQ(foreground::select_target({false, true, false, true}), foreground::target_selection_e::owned_process_group);
  }

  TEST(ForegroundSuspendSelection, UsesForegroundForPlaceholderMissingOrLauncherGroup) {
    EXPECT_EQ(foreground::select_target({true, false, false, true}), foreground::target_selection_e::foreground_process);
    EXPECT_EQ(foreground::select_target({false, false, false, true}), foreground::target_selection_e::foreground_process);
    EXPECT_EQ(foreground::select_target({false, true, true, true}), foreground::target_selection_e::foreground_process);
  }

  TEST(ForegroundSuspendSelection, RejectedLauncherClosedGameOrMissingWindowSelectsNothing) {
    EXPECT_EQ(foreground::select_target({true, false, false, false}), foreground::target_selection_e::none);
    EXPECT_EQ(foreground::select_target({false, true, true, false}), foreground::target_selection_e::none);
    EXPECT_EQ(foreground::select_target({false, false, false, false}), foreground::target_selection_e::none);
  }

  TEST(ForegroundSuspendSelection, ForegroundGameOutsideLauncherOwnedGroupSelectsOneProcess) {
    EXPECT_EQ(foreground::select_target({false, true, true, true}), foreground::target_selection_e::foreground_process);
  }

  TEST(DisconnectSuspensionLifecycle, StoresExactTargetAndRepeatedAttachIsIdempotent) {
    int first_resumes = 0;
    int second_resumes = 0;
    foreground::foreground_target_slot_t slot;
    EXPECT_TRUE(slot.attach(foreground::target_t::for_tests(100, "game.exe", [&]() {
      ++first_resumes;
      return foreground::resume_result_e::resumed;
    })));
    ASSERT_TRUE(slot.target());
    EXPECT_EQ(slot.target()->pid(), 100u);
    EXPECT_FALSE(slot.attach(foreground::target_t::for_tests(200, "other.exe", [&]() {
      ++second_resumes;
      return foreground::resume_result_e::resumed;
    })));
    EXPECT_EQ(slot.target()->pid(), 100u);
    EXPECT_EQ(first_resumes, 0);
    EXPECT_EQ(second_resumes, 1);
  }

  TEST(DisconnectSuspensionLifecycle, ReconnectResumesOnceAndClearsState) {
    int resumes = 0;
    foreground::foreground_target_slot_t slot;
    ASSERT_TRUE(slot.attach(foreground::target_t::for_tests(100, "game.exe", [&]() {
      ++resumes;
      return foreground::resume_result_e::resumed;
    })));
    EXPECT_EQ(slot.recover(), foreground::resume_result_e::resumed);
    EXPECT_FALSE(slot.active());
    EXPECT_EQ(slot.recover(), foreground::resume_result_e::resumed);
    EXPECT_EQ(resumes, 1);
  }

  TEST(DisconnectSuspensionLifecycle, ExitedTargetIsRecovered) {
    foreground::foreground_target_slot_t slot;
    ASSERT_TRUE(slot.attach(foreground::target_t::for_tests(100, "game.exe", []() {
      return foreground::resume_result_e::exited;
    })));
    EXPECT_EQ(slot.recover(), foreground::resume_result_e::exited);
    EXPECT_FALSE(slot.active());
  }

  TEST(DisconnectSuspensionLifecycle, ResumeFailureRetainsTargetForRetryAndBlocksReplacement) {
    int attempts = 0;
    foreground::foreground_target_slot_t slot;
    ASSERT_TRUE(slot.attach(foreground::target_t::for_tests(100, "game.exe", [&]() {
      ++attempts;
      return attempts == 1 ? foreground::resume_result_e::failed : foreground::resume_result_e::resumed;
    })));
    EXPECT_EQ(slot.recover(), foreground::resume_result_e::failed);
    EXPECT_TRUE(slot.active());
    EXPECT_FALSE(slot.attach(foreground::target_t::for_tests(200, "other.exe", []() {
      return foreground::resume_result_e::resumed;
    })));
    EXPECT_EQ(slot.recover(), foreground::resume_result_e::resumed);
    EXPECT_FALSE(slot.active());
    EXPECT_EQ(attempts, 2);
  }
}  // namespace
#endif

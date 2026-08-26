#include "../tests_common.h"

#include "src/remote_session.h"

#include <atomic>
#include <algorithm>
#include <thread>

namespace {
  remote_session::caller_t caller(std::string uuid, bool view = true, bool launch = true, bool terminate = true) {
    return {.uuid = std::move(uuid), .paired = true, .may_view = view, .may_launch = launch, .may_terminate = terminate};
  }
  remote_session::game_t game() { return {.running = true, .owner_uuid = "owner", .generation = 7, .app = {42, "game", "Running game", false}}; }
}

TEST(RemoteSession, SyntheticIdsAndLegacyIdsNeverFallThrough) {
  EXPECT_EQ(remote_session::identify(remote_session::monitor_id), remote_session::control_e::monitor);
  EXPECT_EQ(remote_session::identify(2147483605), remote_session::control_e::monitor);
  EXPECT_EQ(remote_session::identify(11, "9a1c5a25-58fe-40e0-b9aa-7d3f00000006"), remote_session::control_e::input);
  EXPECT_EQ(remote_session::identify(remote_session::running_game_id), remote_session::control_e::running_game);
  const auto running_id = remote_session::synthetic_running_game_id(42);
  EXPECT_NE(running_id, 42);
  EXPECT_EQ(remote_session::identify(running_id), remote_session::control_e::none);
  EXPECT_EQ(remote_session::identify(running_id, {}, 42), remote_session::control_e::running_game);
  EXPECT_EQ(remote_session::identify(running_id, {}, 43), remote_session::control_e::none);
  EXPECT_EQ(remote_session::identify(11), remote_session::control_e::none);
  EXPECT_TRUE(remote_session::reserved_name("remote monitor"));
  EXPECT_TRUE(remote_session::reserved_name("    Remote Monitor"));
  EXPECT_TRUE(remote_session::reserved_name("Remote Input"));
  EXPECT_TRUE(remote_session::reserved_name("Virtual Display"));
  EXPECT_TRUE(remote_session::reserved_name("   Remote Input"));
  EXPECT_TRUE(remote_session::reserved_name("Terminate"));
  EXPECT_EQ(remote_session::synthetic(remote_session::control_e::monitor).title, "Remote Monitor");
  EXPECT_EQ(remote_session::synthetic(remote_session::control_e::input).title, "Remote Input");
  EXPECT_EQ(remote_session::synthetic(remote_session::control_e::resume).title, "Resume");
  EXPECT_EQ(remote_session::synthetic(remote_session::control_e::terminate).title, "Terminate");
  ASSERT_TRUE(remote_session::synthetic_artwork_filename(remote_session::control_e::monitor));
  EXPECT_EQ(*remote_session::synthetic_artwork_filename(remote_session::control_e::monitor), "remote-monitor.png");
  ASSERT_TRUE(remote_session::synthetic_artwork_filename(remote_session::control_e::disconnect_monitor));
  EXPECT_EQ(*remote_session::synthetic_artwork_filename(remote_session::control_e::disconnect_monitor), "disconnect-remote-monitor.png");
  ASSERT_TRUE(remote_session::synthetic_artwork_filename(remote_session::control_e::terminate));
  EXPECT_EQ(*remote_session::synthetic_artwork_filename(remote_session::control_e::terminate), "terminate.png");
  EXPECT_FALSE(remote_session::synthetic_artwork_filename(remote_session::control_e::none));
}

TEST(RemoteSession, CatalogueProjectionMatchesCallerOwnershipMatrix) {
  const std::vector<remote_session::app_t> configured {{1, "one", "One", false}, {2, "two", "Two", false}};
  const auto idle = remote_session::project(caller("other"), {}, {}, configured, false);
  ASSERT_TRUE(idle.free);
  ASSERT_EQ(idle.catalogue.size(), 4);
  EXPECT_EQ(idle.catalogue[2].id, remote_session::input_id);
  EXPECT_EQ(idle.catalogue[3].id, remote_session::monitor_id);

  const auto owner = remote_session::project(caller("owner"), game(), {}, configured, false);
  EXPECT_FALSE(owner.free);
  EXPECT_EQ(owner.current_game, 42);
  ASSERT_EQ(owner.catalogue.size(), 4);
  EXPECT_EQ(owner.catalogue[0].title, "One");

  const auto normal_observer = remote_session::project(caller("other"), game(), {}, configured, false);
  EXPECT_TRUE(normal_observer.free);
  EXPECT_EQ(normal_observer.current_game, 0);
  ASSERT_EQ(normal_observer.catalogue.size(), 7);
  EXPECT_EQ(normal_observer.catalogue[0].id, remote_session::synthetic_running_game_id(42));
  EXPECT_EQ(normal_observer.catalogue[1].id, remote_session::secondary_resume_id);
  EXPECT_EQ(normal_observer.catalogue[2].id, remote_session::secondary_terminate_id);
  EXPECT_EQ(normal_observer.catalogue[3].title, "One");
  EXPECT_EQ(normal_observer.catalogue[4].title, "Two");
  EXPECT_EQ(normal_observer.catalogue[5].id, remote_session::secondary_input_id);
  EXPECT_EQ(normal_observer.catalogue[6].id, remote_session::secondary_monitor_id);

  const auto observer = remote_session::project(caller("other"), game(), {}, configured, true);
  ASSERT_EQ(observer.catalogue.size(), 7);
  EXPECT_EQ(observer.catalogue[0].id, remote_session::synthetic_running_game_id(42));
  EXPECT_EQ(observer.catalogue[1].id, remote_session::secondary_resume_id);
  EXPECT_EQ(observer.catalogue[2].id, remote_session::secondary_terminate_id);
  EXPECT_EQ(observer.catalogue[2].title, " Terminate");
  EXPECT_EQ(observer.catalogue[3].title, "One");
  EXPECT_EQ(observer.catalogue[4].title, "Two");
  EXPECT_EQ(observer.catalogue[5].id, remote_session::secondary_input_id);
  EXPECT_EQ(observer.catalogue[6].id, remote_session::secondary_monitor_id);

  const auto monitor = remote_session::project(caller("monitor"), {}, {.role = remote_session::role_e::monitor, .retained = true}, configured, true);
  ASSERT_EQ(monitor.catalogue.size(), 2);
  EXPECT_EQ(monitor.catalogue[0].id, remote_session::resume_id);
  EXPECT_EQ(monitor.catalogue[1].id, remote_session::disconnect_monitor_id);

  const auto input = remote_session::project(caller("input"), {}, {.role = remote_session::role_e::input}, configured, true);
  ASSERT_EQ(input.catalogue.size(), 3);
  EXPECT_EQ(input.catalogue[0].id, 1);
  EXPECT_EQ(input.catalogue[1].id, 2);
  EXPECT_EQ(input.catalogue[2].id, remote_session::monitor_id);

  const auto input_during_game = remote_session::project(caller("input"), game(), {.role = remote_session::role_e::input}, configured, true);
  ASSERT_EQ(input_during_game.catalogue.size(), 6);
  EXPECT_EQ(input_during_game.catalogue[0].id, remote_session::synthetic_running_game_id(42));
  EXPECT_EQ(input_during_game.catalogue[1].id, remote_session::secondary_resume_id);
  EXPECT_EQ(input_during_game.catalogue[2].id, remote_session::secondary_terminate_id);
  EXPECT_EQ(input_during_game.catalogue[3].title, "One");
  EXPECT_EQ(input_during_game.catalogue[4].title, "Two");
  EXPECT_EQ(input_during_game.catalogue[5].id, remote_session::secondary_monitor_id);

  const auto game_owner_monitor = remote_session::project(caller("owner"), game(), {.role = remote_session::role_e::monitor, .retained = true}, configured, true);
  ASSERT_EQ(game_owner_monitor.catalogue.size(), 2);
  EXPECT_EQ(game_owner_monitor.catalogue[0].id, remote_session::resume_id);
  EXPECT_EQ(game_owner_monitor.catalogue[1].id, remote_session::disconnect_monitor_id);
}

TEST(RemoteSession, ConfiguredRemoteMarkersCannotShadowSyntheticControls) {
  const auto configured_input = remote_session::synthetic(remote_session::control_e::input);
  const auto configured_monitor = remote_session::synthetic(remote_session::control_e::monitor);
  const std::vector<remote_session::app_t> configured {
    {1, "one", "One", false},
    {2, "shadow-input", "Remote Input", false},
    {3, "shadow-monitor", "remote monitor", false},
    {4, "legacy-virtual-display", "Virtual Display", false},
    {5, configured_input.uuid, configured_input.title, false},
    {6, configured_monitor.uuid, configured_monitor.title, false},
  };
  const auto idle = remote_session::project(caller("client"), {}, {}, configured, false);
  ASSERT_EQ(idle.catalogue.size(), 3);
  EXPECT_EQ(idle.catalogue[0].title, "One");
  EXPECT_EQ(idle.catalogue[1].id, remote_session::input_id);
  EXPECT_EQ(idle.catalogue[2].id, remote_session::monitor_id);
}

TEST(RemoteSession, UngatedGameStaysLaunchableAndCancelableUntilSpecialSessionOwnership) {
  const auto active_game = game();
  const auto owner = caller("owner");
  const auto other = caller("other");

  EXPECT_TRUE(remote_session::exposes_active_game(owner, active_game, {}, false));
  EXPECT_FALSE(remote_session::exposes_active_game(other, active_game, {}, false));
  EXPECT_TRUE(remote_session::allows_normal_game_cancel(owner, active_game, false));
  EXPECT_TRUE(remote_session::allows_normal_game_cancel(other, active_game, false));

  EXPECT_TRUE(remote_session::exposes_active_game(owner, active_game, {}, true));
  EXPECT_FALSE(remote_session::exposes_active_game(other, active_game, {}, true));
  EXPECT_TRUE(remote_session::exposes_active_game(other, active_game, {}, false, true));
  EXPECT_TRUE(remote_session::allows_normal_game_cancel(owner, active_game, true));
  EXPECT_FALSE(remote_session::allows_normal_game_cancel(other, active_game, true));

  const remote_session::owner_t retained_monitor {.role = remote_session::role_e::monitor, .retained = true};
  EXPECT_FALSE(remote_session::exposes_active_game(owner, active_game, retained_monitor, true));
  EXPECT_FALSE(remote_session::exposes_active_game(other, {}, {}, false));
  EXPECT_FALSE(remote_session::allows_normal_game_cancel(caller("other", true, true, false), active_game, false));
}

TEST(RemoteSession, SecondaryCatalogueKeepsConfiguredRunningAppBesideInvisibleResumeDuplicate) {
  const auto active_game = game();
  const std::vector<remote_session::app_t> configured {
    active_game.app,
    {7, "other", "Another game", false},
  };

  const auto projection = remote_session::project(caller("other"), active_game, {}, configured, false);
  ASSERT_EQ(projection.catalogue.size(), 7);
  EXPECT_EQ(projection.catalogue[0].id, remote_session::synthetic_running_game_id(active_game.app.id));
  EXPECT_EQ(projection.catalogue[0].uuid, remote_session::synthetic_uuid(remote_session::control_e::running_game));
  EXPECT_EQ(projection.catalogue[0].title, "     Running game");
  EXPECT_EQ(projection.catalogue[1].id, remote_session::secondary_resume_id);
  EXPECT_EQ(projection.catalogue[3].id, active_game.app.id);
  EXPECT_EQ(projection.catalogue[3].uuid, active_game.app.uuid);
  EXPECT_EQ(projection.catalogue[3].title, active_game.app.title);
  EXPECT_EQ(
    remote_session::dispatch(caller("other"), active_game, {}, remote_session::identify(projection.catalogue[0].id, projection.catalogue[0].uuid)).resume_role,
    remote_session::role_e::game
  );
}

TEST(RemoteSession, OnlySecondaryCatalogueControlsReceiveAlphabeticalPriority) {
  const remote_session::app_t first {42, "first", "Zebra", false};
  const remote_session::app_t second {77, "second", "Alpha", false};
  const auto first_tile = remote_session::synthetic_running_game(first);
  const auto repeated_first_tile = remote_session::synthetic_running_game(first);
  const auto second_tile = remote_session::synthetic_running_game(second);

  EXPECT_EQ(first_tile.id, repeated_first_tile.id);
  EXPECT_NE(first_tile.id, first.id);
  EXPECT_NE(first_tile.id, second_tile.id);

  const std::vector<remote_session::app_t> configured {{1, "ordinary", "Another game", false}};
  auto primary = remote_session::project(caller("first"), {.running = true, .owner_uuid = "first", .app = first}, {}, configured, false).catalogue;
  std::sort(primary.begin(), primary.end(), [](const auto &left, const auto &right) {
    return left.title < right.title;
  });
  EXPECT_EQ(primary[0].id, 1);
  EXPECT_EQ(primary[1].id, remote_session::input_id);
  EXPECT_EQ(primary[2].id, remote_session::monitor_id);

  auto secondary = remote_session::project(caller("other"), {.running = true, .owner_uuid = "first", .app = first}, {}, configured, false).catalogue;
  std::sort(secondary.begin(), secondary.end(), [](const auto &left, const auto &right) {
    return left.title < right.title;
  });
  EXPECT_EQ(secondary[0].id, first_tile.id);
  EXPECT_EQ(secondary[1].id, remote_session::secondary_monitor_id);
  EXPECT_EQ(secondary[2].id, remote_session::secondary_input_id);
  EXPECT_EQ(secondary[3].id, remote_session::secondary_resume_id);
  EXPECT_EQ(secondary[4].id, remote_session::secondary_terminate_id);
  EXPECT_EQ(secondary[5].id, 1);
}

TEST(RemoteSession, CatalogueTransitionsDoNotDuplicateMoonlightRows) {
  const std::vector<remote_session::app_t> configured {
    {1, "alpha", "Alpha", false}, {2, "beta", "Beta", false}, game().app,
  };
  const std::vector<remote_session::projection_t> states {
    remote_session::project(caller("other"), {}, {}, configured, false),
    remote_session::project(caller("owner"), game(), {}, configured, false),
    remote_session::project(caller("other"), game(), {}, configured, false),
    remote_session::project(caller("other"), game(), {}, configured, true),
    remote_session::project(caller("other"), {}, {.role = remote_session::role_e::input}, configured, true),
    remote_session::project(caller("other"), game(), {.role = remote_session::role_e::input}, configured, true),
    remote_session::project(caller("other"), game(), {.role = remote_session::role_e::monitor, .retained = true}, configured, true),
  };
  // These ASCII titles have the same order with Moonlight's lowercase sort.
  const auto sort = [](auto &apps) {
    std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) { return a.title < b.title; });
  };
  for (std::size_t from = 0; from < states.size(); ++from) {
    for (std::size_t to = 0; to < states.size(); ++to) {
      SCOPED_TRACE(std::to_string(from) + " -> " + std::to_string(to));
      auto visible = states[from].catalogue;
      auto next = states[to].catalogue;
      sort(visible);
      sort(next);
      // Reproduce Moonlight Qt AppModel's incremental update contract:
      // surviving IDs are renamed in place, then additions use sorted search.
      for (auto it = visible.begin(); it != visible.end();) {
        const auto found = std::find_if(next.begin(), next.end(), [&](const auto &app) { return app.id == it->id; });
        if (found == next.end()) {
          it = visible.erase(it);
        } else {
          EXPECT_EQ(it->title, found->title) << "Surviving IDs must not change sort position";
          *it++ = *found;
        }
      }
      for (const auto &app : next) {
        const auto pos = std::find_if(visible.begin(), visible.end(), [&](const auto &existing) {
          return existing.id == app.id || existing.title > app.title;
        });
        if (pos == visible.end() || pos->id != app.id) visible.insert(pos, app);
      }
      ASSERT_EQ(visible.size(), next.size());
      for (std::size_t i = 0; i < next.size(); ++i) {
        EXPECT_EQ(visible[i].id, next[i].id);
        EXPECT_EQ(visible[i].title, next[i].title);
      }
    }
  }
}

TEST(RemoteSession, PrioritizedIdentitiesPreserveActionsAndArtwork) {
  const auto active_game = game();
  const auto other = caller("other");
  const auto projection = remote_session::project(other, active_game, {}, {}, true);
  for (const auto &app : projection.catalogue) {
    const auto control = remote_session::identify(app.id, {}, active_game.app.id);
    ASSERT_NE(control, remote_session::control_e::none);
    EXPECT_EQ(control, remote_session::identify(0, app.uuid));
    if (control == remote_session::control_e::running_game) continue;
    const auto canonical = remote_session::synthetic(control);
    EXPECT_NE(app.id, canonical.id);
    EXPECT_EQ(app.uuid, canonical.uuid);  // Preserve custom covers and UUID launches.
    const auto actual = remote_session::dispatch(other, active_game, {}, control);
    const auto expected = remote_session::dispatch(other, active_game, {}, remote_session::identify(canonical.id));
    EXPECT_EQ(actual.allowed, expected.allowed);
    EXPECT_EQ(actual.permission, expected.permission);
    EXPECT_EQ(actual.resume, expected.resume);
    EXPECT_EQ(actual.resume_role, expected.resume_role);
    EXPECT_EQ(actual.terminate, expected.terminate);
    EXPECT_EQ(remote_session::synthetic_artwork_filename(control), remote_session::synthetic_artwork_filename(remote_session::identify(canonical.id)));
    EXPECT_NE(remote_session::synthetic_running_game_id(app.id ^ 0x40000000), app.id);
  }
}

TEST(RemoteSession, DispatchEnforcesCallerPermissionsAndRetention) {
  const auto active_game = game();
  const auto game_resume = remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::resume);
  EXPECT_TRUE(game_resume.allowed);
  EXPECT_EQ(game_resume.resume_role, remote_session::role_e::game);
  EXPECT_FALSE(remote_session::dispatch(caller("other", false), active_game, {}, remote_session::control_e::resume).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::terminate).terminate);
  EXPECT_TRUE(remote_session::dispatch(caller("owner"), active_game, {}, remote_session::control_e::terminate).terminate);
  EXPECT_FALSE(remote_session::dispatch(caller("other", true, true, false), active_game, {}, remote_session::control_e::terminate).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::disconnect_monitor).allowed);
  const auto ownerless_disconnect = remote_session::dispatch(caller("foreign"), {}, {}, remote_session::control_e::disconnect_monitor);
  EXPECT_TRUE(ownerless_disconnect.allowed);
  EXPECT_TRUE(ownerless_disconnect.already_complete);
  EXPECT_FALSE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::input).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("input"), {}, {.role = remote_session::role_e::input}, remote_session::control_e::monitor).allowed);
  const auto stale_monitor_launch = remote_session::dispatch(
    caller("monitor"),
    {},
    {.role = remote_session::role_e::monitor, .retained = true},
    remote_session::control_e::monitor
  );
  EXPECT_TRUE(stale_monitor_launch.allowed);
  EXPECT_TRUE(stale_monitor_launch.resume);
  EXPECT_EQ(stale_monitor_launch.resume_role, remote_session::role_e::monitor);
  EXPECT_FALSE(remote_session::input_uses_display_or_audio(remote_session::role_e::input));
  EXPECT_TRUE(remote_session::input_uses_display_or_audio(remote_session::role_e::monitor));
  EXPECT_FALSE(remote_session::uses_audio(remote_session::role_e::input, false));
  EXPECT_TRUE(remote_session::uses_audio(remote_session::role_e::monitor, false));
  EXPECT_FALSE(remote_session::uses_audio(remote_session::role_e::monitor, true));
  EXPECT_TRUE(remote_session::uses_audio(remote_session::role_e::game, true));
  EXPECT_FALSE(remote_session::uses_host_audio(remote_session::role_e::input));
  EXPECT_TRUE(remote_session::uses_host_audio(remote_session::role_e::monitor));
}

TEST(RemoteSession, MonitorDisconnectPoliciesKeepStreamEndAndClientLossIndependent) {
  EXPECT_FALSE(remote_session::disconnect_monitor_after_stream(false, false, false));
  EXPECT_FALSE(remote_session::disconnect_monitor_after_stream(false, true, false));
  EXPECT_TRUE(remote_session::disconnect_monitor_after_stream(false, true, true));
  EXPECT_TRUE(remote_session::disconnect_monitor_after_stream(true, false, false));
  EXPECT_TRUE(remote_session::disconnect_monitor_after_stream(true, true, true));
}

TEST(RemoteSession, StaleDisconnectControlsAreIdempotentButCannotTargetAnotherRole) {
  const auto stale_input = remote_session::dispatch(caller("input"), {}, {}, remote_session::control_e::disconnect_input);
  EXPECT_TRUE(stale_input.allowed);
  EXPECT_TRUE(stale_input.already_complete);
  const auto stale_monitor = remote_session::dispatch(caller("monitor"), {}, {}, remote_session::control_e::disconnect_monitor);
  EXPECT_TRUE(stale_monitor.allowed);
  EXPECT_TRUE(stale_monitor.already_complete);

  EXPECT_FALSE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::disconnect_input).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("input"), {}, {.role = remote_session::role_e::input}, remote_session::control_e::disconnect_monitor).allowed);
}

TEST(RemoteSession, SecondaryGameTransportJoinsActiveOrRetainedOutput) {
  EXPECT_TRUE(remote_session::joins_existing_game_output(remote_session::role_e::game, true));
  EXPECT_FALSE(remote_session::joins_existing_game_output(remote_session::role_e::game, false));
  EXPECT_TRUE(remote_session::joins_existing_game_output(remote_session::role_e::game, false, true));
  EXPECT_FALSE(remote_session::joins_existing_game_output(remote_session::role_e::monitor, true));
  EXPECT_FALSE(remote_session::joins_existing_game_output(remote_session::role_e::monitor, false, true));
  EXPECT_FALSE(remote_session::joins_existing_game_output(remote_session::role_e::input, true));
  EXPECT_FALSE(remote_session::joins_existing_game_output(remote_session::role_e::input, false, true));
}

TEST(RemoteSession, ApplistResumeUsesLaunchResponseShape) {
  EXPECT_EQ(remote_session::stream_start_response_key(true), "gamesession");
  EXPECT_EQ(remote_session::stream_start_response_key(false), "resume");
}

TEST(RemoteSession, RetainedMonitorResumeWinsOverPausedConfiguredApp) {
  const auto decision = remote_session::dispatch(
    caller("monitor"),
    game(),
    {.role = remote_session::role_e::monitor, .retained = true},
    remote_session::control_e::resume
  );

  ASSERT_TRUE(decision.allowed);
  ASSERT_TRUE(decision.resume);
  EXPECT_EQ(decision.resume_role, remote_session::role_e::monitor);
}

TEST(RemoteSession, CapturePlanNeverFallsBackForSpecialRoles) {
  const auto input = remote_session::capture_plan(remote_session::role_e::input, std::string {"\\\\.\\DISPLAY99"});
  EXPECT_EQ(input.source, remote_session::capture_source_e::synthetic_black);
  EXPECT_FALSE(input.output);

  const auto monitor = remote_session::capture_plan(remote_session::role_e::monitor, std::string {"\\\\.\\DISPLAY54"});
  EXPECT_EQ(monitor.source, remote_session::capture_source_e::exact_output);
  ASSERT_TRUE(monitor.output);
  EXPECT_EQ(*monitor.output, "\\\\.\\DISPLAY54");

  EXPECT_EQ(remote_session::capture_plan(remote_session::role_e::monitor).source, remote_session::capture_source_e::invalid);
  EXPECT_EQ(remote_session::capture_plan(remote_session::role_e::monitor, std::string {}).source, remote_session::capture_source_e::invalid);
  EXPECT_EQ(remote_session::capture_plan(remote_session::role_e::game).source, remote_session::capture_source_e::active_output);
}

TEST(RemoteSession, ConvertsApolloSessionMillihertzBeforeCreatingDisplayModes) {
  EXPECT_EQ(remote_session::display_refresh_hz_from_session_fps(120000), 120);
  EXPECT_EQ(remote_session::display_refresh_hz_from_session_fps(119880), 120);
  EXPECT_EQ(remote_session::display_refresh_hz_from_session_fps(59940), 60);
  EXPECT_EQ(remote_session::display_refresh_hz_from_session_fps(60), 60);
  EXPECT_EQ(remote_session::monitor_mode_from_session_fps(2560, 1440, 120000), "2560x1440@120");
}

TEST(RemoteSession, DisconnectControlsCompleteAsDisplayedLaunchFailures) {
  const auto monitor = remote_session::successful_control_completion(remote_session::control_e::disconnect_monitor);
  ASSERT_TRUE(monitor);
  EXPECT_EQ(monitor->status_code, 410);
  EXPECT_EQ(monitor->status_message, "Remote Monitor disconnected successfully.");

  const auto input = remote_session::successful_control_completion(remote_session::control_e::disconnect_input);
  ASSERT_TRUE(input);
  EXPECT_EQ(input->status_code, 410);

  const auto game_disconnect = remote_session::successful_control_completion(remote_session::control_e::terminate);
  ASSERT_TRUE(game_disconnect);
  EXPECT_EQ(game_disconnect->status_code, 410);
  EXPECT_EQ(game_disconnect->status_message, "Active stream terminated. Remote Monitor and Remote Input remain connected.");
  EXPECT_EQ(
    remote_session::termination_confirmation_message(),
    "This will close the active stream but leave Remote Monitor and Remote Input connected. Launch Terminate again within 60 seconds to confirm this was intentional."
  );

  EXPECT_FALSE(remote_session::successful_control_completion(remote_session::control_e::resume));
  EXPECT_FALSE(remote_session::successful_control_completion(remote_session::control_e::monitor));
}

TEST(RemoteSession, TerminationConfirmationOnlyProtectsExtraClientsByDefault) {
  EXPECT_TRUE(remote_session::requires_termination_confirmation(false, false));
  EXPECT_FALSE(remote_session::requires_termination_confirmation(false, true));
  EXPECT_FALSE(remote_session::requires_termination_confirmation(true, false));
  EXPECT_FALSE(remote_session::requires_termination_confirmation(true, true));
}

TEST(RemoteSession, TerminateAllowsMobileCatalogueRefreshWithinSixtySeconds) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 7, 42, start),
    remote_session::terminate_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("other", 7, 42, start + std::chrono::seconds {1}),
    remote_session::terminate_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 7, 42, start + std::chrono::seconds {2}),
    remote_session::terminate_confirmation_e::confirmed
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 7, 42, start + std::chrono::seconds {3}),
    remote_session::terminate_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 8, 42, start + std::chrono::seconds {4}),
    remote_session::terminate_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 8, 42, start + std::chrono::seconds {45}),
    remote_session::terminate_confirmation_e::confirmed
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 8, 42, start + std::chrono::seconds {46}),
    remote_session::terminate_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_termination("client", 8, 42, start + std::chrono::seconds {107}),
    remote_session::terminate_confirmation_e::prompt
  );
  remote_session::clear_termination_confirmation("client");
  remote_session::clear_termination_confirmation("other");
}

TEST(RemoteSession, AppReplacementConfirmationIsPerClientGameAndRequestedApp) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(
    remote_session::arm_or_confirm_app_replacement("client", 7, 99, start),
    remote_session::app_replacement_confirmation_e::prompt
  );
  EXPECT_TRUE(remote_session::app_replacement_confirmation_active("client", 7, start + std::chrono::seconds {1}));
  EXPECT_FALSE(remote_session::app_replacement_confirmation_active("other", 7, start + std::chrono::seconds {1}));

  EXPECT_EQ(
    remote_session::arm_or_confirm_app_replacement("client", 7, 100, start + std::chrono::seconds {2}),
    remote_session::app_replacement_confirmation_e::prompt
  );
  EXPECT_EQ(
    remote_session::arm_or_confirm_app_replacement("client", 7, 100, start + std::chrono::seconds {3}),
    remote_session::app_replacement_confirmation_e::confirmed
  );
  EXPECT_FALSE(remote_session::app_replacement_confirmation_active("client", 7, start + std::chrono::seconds {4}));

  EXPECT_EQ(
    remote_session::arm_or_confirm_app_replacement("client", 8, 101, start + std::chrono::seconds {5}),
    remote_session::app_replacement_confirmation_e::prompt
  );
  EXPECT_FALSE(remote_session::app_replacement_confirmation_active("client", 7, start + std::chrono::seconds {6}));
  EXPECT_EQ(
    remote_session::arm_or_confirm_app_replacement("client", 8, 101, start + std::chrono::seconds {7}),
    remote_session::app_replacement_confirmation_e::prompt
  );
  EXPECT_FALSE(remote_session::app_replacement_confirmation_active("client", 8, start + std::chrono::seconds {68}));

  EXPECT_EQ(
    remote_session::app_replacement_confirmation_message(),
    "An app is already running. Launch this app again within 60 seconds to confirm that you want to close it."
  );
  remote_session::clear_app_replacement_confirmation("client");
}

TEST(RemoteSession, PendingRegistryKeepsEncryptedLaunchesDistinctAndPlaintextSafe) {
  remote_session::pending_registry_t registry;
  const auto expiry = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  EXPECT_TRUE(registry.add({.launch_id = 1, .client_uuid = "one", .crypto_binding = "cert-one", .source_address = "10.0.0.1", .encrypted = true, .role = remote_session::role_e::game, .generation = 1, .expires_at = expiry}));
  EXPECT_TRUE(registry.add({.launch_id = 2, .client_uuid = "two", .crypto_binding = "cert-two", .source_address = "10.0.0.1", .encrypted = true, .role = remote_session::role_e::game, .generation = 1, .expires_at = expiry}));
  EXPECT_EQ(registry.match_encrypted("one", "cert-one", std::chrono::steady_clock::now())->launch_id, 1u);
  EXPECT_EQ(registry.match_encrypted("two", "cert-two", std::chrono::steady_clock::now())->launch_id, 2u);
  EXPECT_FALSE(registry.match_encrypted("one", "cert-two", std::chrono::steady_clock::now()));
  EXPECT_TRUE(registry.add({.launch_id = 3, .client_uuid = "three", .source_address = "10.0.0.2", .encrypted = false, .role = remote_session::role_e::game, .expires_at = expiry}));
  std::string warning;
  EXPECT_FALSE(registry.add({.launch_id = 4, .client_uuid = "four", .source_address = "10.0.0.2", .encrypted = false, .role = remote_session::role_e::game, .expires_at = expiry}, &warning));
  EXPECT_FALSE(warning.empty());
  EXPECT_EQ(registry.match_plaintext("10.0.0.2", std::chrono::steady_clock::now())->launch_id, 3u);
  registry.expire(std::chrono::steady_clock::now() + std::chrono::minutes(2));
  EXPECT_FALSE(registry.match_encrypted("one", "cert-one", std::chrono::steady_clock::now() + std::chrono::minutes(2)));
}

TEST(RemoteSession, NormalAppTransitionGateSerializesProcessStartPublication) {
  remote_session::normal_app_transition_gate_t gate;
  std::atomic_bool contender_started {false};
  std::atomic_bool contender_entered {false};
  std::unique_lock first_transition {gate};
  std::jthread contender {[&] {
    contender_started.store(true, std::memory_order_release);
    std::lock_guard second_transition {gate};
    contender_entered.store(true, std::memory_order_release);
  }};
  while (!contender_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(contender_entered.load(std::memory_order_acquire));
  first_transition.unlock();
  contender.join();
  EXPECT_TRUE(contender_entered.load(std::memory_order_acquire));
}

TEST(RemoteSession, MonitorHooksRejectWithoutTopologyAndPreserveGeneration) {
  remote_session::register_monitor_runtime_hooks({});
  const auto unavailable = remote_session::activate_or_resume_monitor("client", "Client", "1920x1080@60", true, 7);
  EXPECT_FALSE(unavailable.accepted);
  EXPECT_TRUE(unavailable.retryable);

  std::uint64_t released_generation {};
  remote_session::register_monitor_runtime_hooks({
    .explicit_release = [&released_generation](std::string_view, std::uint64_t generation, std::string_view) { released_generation = generation; },
  });
  remote_session::release_monitor("client", 7, "disconnect");
  EXPECT_EQ(released_generation, 7u);
  remote_session::register_monitor_runtime_hooks({});
}

TEST(RemoteSession, MonitorRuntimeTransportsHdrRequest) {
  bool observed_hdr = false;
  remote_session::register_monitor_runtime_hooks({
    .activate_or_resume = [&observed_hdr](std::string_view, std::string_view, std::string_view, const bool hdr_requested, std::uint64_t) {
      observed_hdr = hdr_requested;
      return remote_session::monitor_runtime_state_t {.accepted = true, .ready = true, .hdr_enabled = hdr_requested};
    },
  });
  const auto result = remote_session::activate_or_resume_monitor("client", "Client", "3840x2160@120", true, 8);
  EXPECT_TRUE(result.ready);
  EXPECT_TRUE(result.hdr_enabled);
  EXPECT_TRUE(observed_hdr);
  remote_session::register_monitor_runtime_hooks({});
}

TEST(RemoteSession, LayoutGraphRejectsInvalidAnchorsCyclesAndDuplicatePrimary) {
  const std::vector<std::string> clients {"a", "b"};
  const std::vector<std::string> physical {"DISPLAY1"};
  std::string error;
  EXPECT_TRUE(remote_session::validate_layout({{"a", "physical", "DISPLAY1", "right", "center", 0, true}, {"b", "client", "a", "below", "start", 8, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "client", "b", "right", "center", 0, false}, {"b", "client", "a", "right", "center", 0, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "physical", "missing", "right", "center", 0, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "physical", "DISPLAY1", "right", "center", 0, true}, {"b", "physical", "DISPLAY1", "right", "center", 0, true}}, clients, physical, &error));
}

TEST(RemoteSession, DisabledInputHidesCatalogueAndRejectsCachedLaunchWithoutBlockingCleanup) {
  auto disabled = caller("client");
  disabled.input_enabled = false;
  const std::vector<remote_session::app_t> configured {{42, "game", "Game", false}};
  for (const auto active_game : {remote_session::game_t {}, game()}) {
    for (bool active_peers : {false, true}) {
      const auto projected = remote_session::project(disabled, active_game, {}, configured, active_peers);
      for (const auto &entry : projected.catalogue) EXPECT_NE(entry.id, remote_session::input_id);
    }
  }
  EXPECT_FALSE(remote_session::dispatch(disabled, {}, {}, remote_session::control_e::input).allowed);
  EXPECT_TRUE(remote_session::dispatch(disabled, {}, {}, remote_session::control_e::monitor).allowed);
  EXPECT_TRUE(remote_session::dispatch(disabled, game(), {}, remote_session::control_e::resume).allowed);
  EXPECT_TRUE(remote_session::dispatch(disabled, {}, {.role = remote_session::role_e::input}, remote_session::control_e::disconnect_input).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("client"), {}, {}, remote_session::control_e::input).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("client", true, false), {}, {}, remote_session::control_e::input).allowed);
}

TEST(RemoteSession, MonitorCommandsHonorClientAndAppPolicyWithoutChangingInputIsolation) {
  using remote_session::role_e;
  for (bool client_allows : {false, true}) {
    for (bool app_allows : {false, true}) {
      EXPECT_FALSE(remote_session::allows_client_commands(role_e::input, client_allows, app_allows));
      EXPECT_EQ(remote_session::allows_client_commands(role_e::monitor, client_allows, app_allows), client_allows && app_allows);
      EXPECT_EQ(remote_session::allows_client_commands(role_e::game, client_allows, app_allows), client_allows && app_allows);
    }
  }
}

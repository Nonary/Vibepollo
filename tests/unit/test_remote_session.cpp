#include "../tests_common.h"

#include "src/remote_session.h"

namespace {
  remote_session::caller_t caller(std::string uuid, bool view = true, bool launch = true, bool terminate = true) {
    return {.uuid = std::move(uuid), .paired = true, .may_view = view, .may_launch = launch, .may_terminate = terminate};
  }
  remote_session::game_t game() { return {.running = true, .owner_uuid = "owner", .app = {42, "game", "Running game", false}}; }
}

TEST(RemoteSession, SyntheticIdsAndLegacyIdsNeverFallThrough) {
  EXPECT_EQ(remote_session::identify(remote_session::monitor_id), remote_session::control_e::monitor);
  EXPECT_EQ(remote_session::identify(2147483605), remote_session::control_e::monitor);
  EXPECT_EQ(remote_session::identify(11, "9a1c5a25-58fe-40e0-b9aa-7d3f00000006"), remote_session::control_e::input);
  EXPECT_EQ(remote_session::identify(11), remote_session::control_e::none);
  EXPECT_TRUE(remote_session::reserved_name("remote monitor"));
  EXPECT_TRUE(remote_session::reserved_name("Remote Input"));
}

TEST(RemoteSession, CatalogueProjectionMatchesCallerOwnershipMatrix) {
  const std::vector<remote_session::app_t> configured {{1, "one", "One", false}, {2, "two", "Two", false}};
  const auto idle = remote_session::project(caller("other"), {}, {}, configured);
  ASSERT_TRUE(idle.free);
  ASSERT_EQ(idle.catalogue.size(), 4);
  EXPECT_EQ(idle.catalogue[2].id, remote_session::input_id);
  EXPECT_EQ(idle.catalogue[3].id, remote_session::monitor_id);

  const auto owner = remote_session::project(caller("owner"), game(), {}, configured);
  EXPECT_FALSE(owner.free);
  EXPECT_EQ(owner.current_game, 42);
  ASSERT_EQ(owner.catalogue.size(), 4);
  EXPECT_EQ(owner.catalogue[0].title, "One");

  const auto observer = remote_session::project(caller("other"), game(), {}, configured);
  ASSERT_EQ(observer.catalogue.size(), 5);
  EXPECT_EQ(observer.catalogue[0].id, remote_session::resume_id);
  EXPECT_EQ(observer.catalogue[1].id, remote_session::disconnect_game_id);
  EXPECT_EQ(observer.catalogue[2].id, 42);
  EXPECT_EQ(observer.catalogue[3].id, remote_session::input_id);
  EXPECT_EQ(observer.catalogue[4].id, remote_session::monitor_id);

  const auto monitor = remote_session::project(caller("monitor"), {}, {.role = remote_session::role_e::monitor, .retained = true}, configured);
  ASSERT_EQ(monitor.catalogue.size(), 2);
  EXPECT_EQ(monitor.catalogue[0].id, remote_session::resume_id);
  EXPECT_EQ(monitor.catalogue[1].id, remote_session::disconnect_monitor_id);

  const auto input = remote_session::project(caller("input"), {}, {.role = remote_session::role_e::input}, configured);
  ASSERT_EQ(input.catalogue.size(), 1);
  EXPECT_EQ(input.catalogue[0].id, remote_session::disconnect_input_id);
}

TEST(RemoteSession, DispatchEnforcesCallerPermissionsAndRetention) {
  const auto active_game = game();
  EXPECT_TRUE(remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::resume).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("other", false), active_game, {}, remote_session::control_e::resume).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::disconnect_game).terminate_game);
  EXPECT_FALSE(remote_session::dispatch(caller("other", true, true, false), active_game, {}, remote_session::control_e::disconnect_game).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::disconnect_monitor).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("foreign"), {}, {}, remote_session::control_e::disconnect_monitor).allowed);
  EXPECT_FALSE(remote_session::input_uses_display_or_audio(remote_session::role_e::input));
  EXPECT_TRUE(remote_session::input_uses_display_or_audio(remote_session::role_e::monitor));
}

TEST(RemoteSession, PendingRegistryKeepsEncryptedLaunchesDistinctAndPlaintextSafe) {
  remote_session::pending_registry_t registry;
  const auto expiry = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  EXPECT_TRUE(registry.add({1, "one", "10.0.0.1", true, remote_session::role_e::monitor, expiry}));
  EXPECT_TRUE(registry.add({2, "two", "10.0.0.1", true, remote_session::role_e::monitor, expiry}));
  EXPECT_EQ(registry.match_encrypted("one", std::chrono::steady_clock::now())->launch_id, 1u);
  EXPECT_EQ(registry.match_encrypted("two", std::chrono::steady_clock::now())->launch_id, 2u);
  EXPECT_TRUE(registry.add({3, "three", "10.0.0.2", false, remote_session::role_e::game, expiry}));
  std::string warning;
  EXPECT_FALSE(registry.add({4, "four", "10.0.0.2", false, remote_session::role_e::game, expiry}, &warning));
  EXPECT_FALSE(warning.empty());
  EXPECT_EQ(registry.match_plaintext("10.0.0.2", std::chrono::steady_clock::now())->launch_id, 3u);
  registry.expire(std::chrono::steady_clock::now() + std::chrono::minutes(2));
  EXPECT_FALSE(registry.match_encrypted("one", std::chrono::steady_clock::now() + std::chrono::minutes(2)));
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

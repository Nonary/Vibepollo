#include "../tests_common.h"

#include "src/remote_session.h"

#include <atomic>
#include <thread>

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
  ASSERT_TRUE(remote_session::synthetic_artwork_filename(remote_session::control_e::monitor));
  EXPECT_EQ(*remote_session::synthetic_artwork_filename(remote_session::control_e::monitor), "remote-monitor.png");
  ASSERT_TRUE(remote_session::synthetic_artwork_filename(remote_session::control_e::disconnect_monitor));
  EXPECT_EQ(*remote_session::synthetic_artwork_filename(remote_session::control_e::disconnect_monitor), "disconnect-remote-monitor.png");
  EXPECT_FALSE(remote_session::synthetic_artwork_filename(remote_session::control_e::none));
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

  const auto game_owner_monitor = remote_session::project(caller("owner"), game(), {.role = remote_session::role_e::monitor, .retained = true}, configured);
  ASSERT_EQ(game_owner_monitor.catalogue.size(), 2);
  EXPECT_EQ(game_owner_monitor.catalogue[0].id, remote_session::resume_id);
  EXPECT_EQ(game_owner_monitor.catalogue[1].id, remote_session::disconnect_monitor_id);
}

TEST(RemoteSession, ConfiguredRemoteMarkersCannotShadowSyntheticControls) {
  const std::vector<remote_session::app_t> configured {
    {1, "one", "One", false},
    {2, "shadow-input", "Remote Input", false},
    {3, "shadow-monitor", "remote monitor", false},
  };
  const auto idle = remote_session::project(caller("client"), {}, {}, configured);
  ASSERT_EQ(idle.catalogue.size(), 3);
  EXPECT_EQ(idle.catalogue[0].title, "One");
  EXPECT_EQ(idle.catalogue[1].id, remote_session::input_id);
  EXPECT_EQ(idle.catalogue[2].id, remote_session::monitor_id);
}

TEST(RemoteSession, DispatchEnforcesCallerPermissionsAndRetention) {
  const auto active_game = game();
  EXPECT_TRUE(remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::resume).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("other", false), active_game, {}, remote_session::control_e::resume).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("other"), active_game, {}, remote_session::control_e::disconnect_game).disconnect_game);
  EXPECT_TRUE(remote_session::dispatch(caller("owner"), active_game, {}, remote_session::control_e::disconnect_game).disconnect_game);
  EXPECT_FALSE(remote_session::dispatch(caller("other", true, true, false), active_game, {}, remote_session::control_e::disconnect_game).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::disconnect_monitor).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("foreign"), {}, {}, remote_session::control_e::disconnect_monitor).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::input).allowed);
  EXPECT_FALSE(remote_session::dispatch(caller("input"), {}, {.role = remote_session::role_e::input}, remote_session::control_e::monitor).allowed);
  EXPECT_TRUE(remote_session::dispatch(caller("monitor"), {}, {.role = remote_session::role_e::monitor}, remote_session::control_e::monitor).allowed);
  EXPECT_FALSE(remote_session::input_uses_display_or_audio(remote_session::role_e::input));
  EXPECT_TRUE(remote_session::input_uses_display_or_audio(remote_session::role_e::monitor));
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
  const auto unavailable = remote_session::activate_or_resume_monitor("client", "Client", "1920x1080@60", 7);
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

TEST(RemoteSession, LayoutGraphRejectsInvalidAnchorsCyclesAndDuplicatePrimary) {
  const std::vector<std::string> clients {"a", "b"};
  const std::vector<std::string> physical {"DISPLAY1"};
  std::string error;
  EXPECT_TRUE(remote_session::validate_layout({{"a", "physical", "DISPLAY1", "right", "center", 0, true}, {"b", "client", "a", "below", "start", 8, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "client", "b", "right", "center", 0, false}, {"b", "client", "a", "right", "center", 0, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "physical", "missing", "right", "center", 0, false}}, clients, physical, &error));
  EXPECT_FALSE(remote_session::validate_layout({{"a", "physical", "DISPLAY1", "right", "center", 0, true}, {"b", "physical", "DISPLAY1", "right", "center", 0, true}}, clients, physical, &error));
}

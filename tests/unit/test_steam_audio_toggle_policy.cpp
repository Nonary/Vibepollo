/**
 * @file tests/unit/test_steam_audio_toggle_policy.cpp
 * @brief Tests for the Steam audio device auto-toggle policy decisions.
 */
#include "../tests_common.h"

#include <src/audio_steam_toggle_policy.h>

using namespace audio::steam_toggle;

TEST(SteamAudioTogglePolicy, DisabledConfigNeverEnables) {
  session_toggle_state_t state;
  session_event_t event {session_event_e::CONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ false);
  EXPECT_EQ(result.action, toggle_action_e::none);
}

TEST(SteamAudioTogglePolicy, ConnectEnablesDevices) {
  session_toggle_state_t state;
  session_event_t event {session_event_e::CONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ true);
  EXPECT_EQ(result.action, toggle_action_e::enable);
  EXPECT_TRUE(result.should_snapshot);
  EXPECT_TRUE(state.devices_enabled);
  EXPECT_TRUE(state.has_snapshot);
}

TEST(SteamAudioTogglePolicy, ConnectWhileAlreadyEnabledIsNoOp) {
  session_toggle_state_t state;
  state.devices_enabled = true;
  session_event_t event {session_event_e::CONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ true);
  EXPECT_EQ(result.action, toggle_action_e::none);
}

TEST(SteamAudioTogglePolicy, DisconnectWithoutPriorEnableIsNoOp) {
  session_toggle_state_t state;
  state.devices_enabled = false;
  session_event_t event {session_event_e::DISCONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ true);
  EXPECT_EQ(result.action, toggle_action_e::none);
}

TEST(SteamAudioTogglePolicy, DisconnectDisablesAndSchedulesRestore) {
  session_toggle_state_t state;
  state.devices_enabled = true;
  state.has_snapshot = true;
  session_event_t event {session_event_e::DISCONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ true);
  EXPECT_EQ(result.action, toggle_action_e::disable);
  EXPECT_TRUE(result.should_restore);
}

TEST(SteamAudioTogglePolicy, DisconnectWithoutSnapshotStillDisables) {
  session_toggle_state_t state;
  state.devices_enabled = true;
  state.has_snapshot = false;
  session_event_t event {session_event_e::DISCONNECT};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ true);
  EXPECT_EQ(result.action, toggle_action_e::disable);
  EXPECT_FALSE(result.should_restore);
}

TEST(SteamAudioTogglePolicy, DisableConfigChangeWhileEnabledDisables) {
  session_toggle_state_t state;
  state.devices_enabled = true;
  state.has_snapshot = true;
  session_event_t event {session_event_e::CONFIG_CHANGED};
  const auto result = evaluate_toggle(state, event, /* config_enabled */ false);
  EXPECT_EQ(result.action, toggle_action_e::disable);
  EXPECT_TRUE(result.should_restore);
  EXPECT_FALSE(state.devices_enabled);
  EXPECT_FALSE(state.has_snapshot);
}

TEST(SteamAudioTogglePolicy, ConnectThenDisconnectRoundTrip) {
  session_toggle_state_t state;
  const auto connect_result = evaluate_toggle(state, session_event_e::CONNECT, /* config_enabled */ true);
  EXPECT_EQ(connect_result.action, toggle_action_e::enable);
  EXPECT_TRUE(connect_result.should_snapshot);

  const auto disconnect_result = evaluate_toggle(state, session_event_e::DISCONNECT, /* config_enabled */ true);
  EXPECT_EQ(disconnect_result.action, toggle_action_e::disable);
  EXPECT_TRUE(disconnect_result.should_restore);
  EXPECT_FALSE(state.devices_enabled);
  EXPECT_FALSE(state.has_snapshot);
}
#include "src/lutris_sync_policy.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <optional>

namespace {
  platf::lutris::game_t game(std::int64_t id, std::string name, std::string runner = "wine") {
    platf::lutris::game_t result;
    result.id = id;
    result.name = std::move(name);
    result.runner = std::move(runner);
    result.directory = "/games/example";
    return result;
  }
}

#if defined(__linux__)
TEST(LutrisSync, MachineHostUpgradesManagedCommandsAndPreservesManualAppsAndHooks) {
  const auto *current = std::getenv("VIBEPOLLO_MACHINE_HOST");
  const auto previous = current ? std::optional<std::string> {current} : std::nullopt;
  setenv("VIBEPOLLO_MACHINE_HOST", "1", 1);
  nlohmann::json root = {{"apps", nlohmann::json::array({
    {{"name", "Old managed"}, {"lutris-id", "16"}, {"lutris-managed", "auto"},
      {"cmd", "lutris lutris:rungameid/16"},
      {"prep-cmd", nlohmann::json::array({{{"do", "user hook"}, {"undo", ""}}})}},
    {{"name", "Manual"}, {"lutris-id", "17"}, {"cmd", "custom launcher"}},
  })}};
  const bool changed = platf::lutris::sync::policy::reconcile(root, {game(16, "Managed"), game(17, "Manual")});
  if (previous) setenv("VIBEPOLLO_MACHINE_HOST", previous->c_str(), 1);
  else unsetenv("VIBEPOLLO_MACHINE_HOST");
  ASSERT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 2U);
  EXPECT_EQ(root["apps"][0]["cmd"], "/usr/libexec/vibeshine/vibepollo-session-exec lutris 16");
  EXPECT_EQ(root["apps"][0]["prep-cmd"][0]["do"], "user hook");
  EXPECT_EQ(root["apps"][1]["cmd"], "custom launcher");
}
#endif

TEST(LutrisSync, ImportsNonSteamAndTracksProviderMetadata) {
  nlohmann::json root = {{"apps", nlohmann::json::array()}};
  ASSERT_TRUE(platf::lutris::sync::policy::reconcile(root, {game(7, "Battle.net")}));
  ASSERT_EQ(root["apps"].size(), 1U);
  EXPECT_EQ(root["apps"][0]["lutris-id"], "7");
  EXPECT_EQ(root["apps"][0]["lutris-directory"], "/games/example");
  EXPECT_EQ(root["apps"][0]["cmd"], "lutris lutris:rungameid/7");
}

TEST(LutrisSync, SteamEntriesAreOffByDefaultAndDirectSteamWins) {
  auto steam_game = game(8, "Steam Copy", "steam");
  steam_game.service = "steam";
  steam_game.service_id = "42";
  nlohmann::json root = {{"apps", nlohmann::json::array()}};
  EXPECT_FALSE(platf::lutris::sync::policy::reconcile(root, {steam_game}));
  EXPECT_TRUE(root["apps"].empty());

  root["apps"].push_back({{"name", "Direct Steam"}, {"steam-id", "42"}, {"steam-managed", "auto"}});
  EXPECT_FALSE(platf::lutris::sync::policy::reconcile(root, {steam_game}, true, {}, true));
  ASSERT_EQ(root["apps"].size(), 1U);
  EXPECT_EQ(root["apps"][0]["name"], "Direct Steam");
}

TEST(LutrisSync, ExclusionsRemoveOnlyManagedEntries) {
  nlohmann::json root = {{"apps", nlohmann::json::array({
    nlohmann::json {{"name", "Managed"}, {"lutris-id", "9"}, {"lutris-managed", "auto"}},
    nlohmann::json {{"name", "Manual"}, {"lutris-id", "9"}},
  })}};
  const std::vector<config::id_name_t> exclusions {{"9", "Managed"}};
  EXPECT_TRUE(platf::lutris::sync::policy::reconcile(root, {}, false, exclusions));
  ASSERT_EQ(root["apps"].size(), 1U);
  EXPECT_EQ(root["apps"][0]["name"], "Manual");
}

TEST(LutrisSync, ManualEntriesRetainCommandsAndReservedSyntaxSelectsOnlyTheFixedOperation) {
  const std::string reserved = "/usr/libexec/vibeshine/vibepollo-session-exec lutris 16";
  const std::string custom = "lutris lutris:rungameid/17 --custom";
  nlohmann::json root = {{"apps", nlohmann::json::array({
    {{"name", "Manual fixed operation"}, {"lutris-id", "16"}, {"cmd", reserved}},
    {{"name", "Manual custom command"}, {"lutris-id", "17"}, {"cmd", custom}},
  })}};
  EXPECT_FALSE(platf::lutris::sync::policy::reconcile(root, {game(16, "First"), game(17, "Second")}));
  EXPECT_EQ(root["apps"][0]["cmd"], reserved);
  EXPECT_EQ(root["apps"][1]["cmd"], custom);
  const auto fixed = platf::lutris::session_launch_arguments(root["apps"][0]["cmd"].get<std::string>());
  ASSERT_TRUE(fixed);
  EXPECT_EQ(*fixed, (std::vector<std::string> {"lutris", "16"}));
  EXPECT_FALSE(platf::lutris::session_launch_arguments(root["apps"][1]["cmd"].get<std::string>()));
}

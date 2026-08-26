#include "src/lutris_sync_policy.h"

#include <gtest/gtest.h>

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

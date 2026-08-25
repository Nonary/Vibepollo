#include "src/steam_sync_policy.h"

#include <gtest/gtest.h>

TEST(SteamSync, PreservesManualAndPlayniteEntries) {
  nlohmann::json root = {{"apps", nlohmann::json::array({
                                    nlohmann::json {{"name", "Manual"}},
                                    nlohmann::json {{"name", "Playnite"}, {"playnite-id", "x"}},
                                  })}};
  platf::steam::game_t game;
  game.app_id = 10;
  game.name = "Steam Game";
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  ASSERT_EQ(root["apps"].size(), 3U);
  EXPECT_EQ(root["apps"][0]["name"], "Manual");
  EXPECT_EQ(root["apps"][1]["name"], "Playnite");
  EXPECT_EQ(root["apps"][2]["steam-id"], "10");
}

TEST(SteamSync, UsesValidDeterministicUuidForSteamEntries) {
  const auto uuid = platf::steam::sync::policy::canonical_steam_app_uuid(42);
  EXPECT_EQ(uuid, "53544541-4d00-5000-8000-00000000002a");
  EXPECT_EQ(platf::steam::sync::policy::canonical_steam_app_uuid(42), uuid);
}

TEST(SteamSync, UpdatesAndRemovesOnlyManagedEntries) {
  nlohmann::json root = {{"apps", nlohmann::json::array({
                                    nlohmann::json {{"name", "Old"}, {"steam-id", "10"}, {"steam-managed", "auto"}},
                                    nlohmann::json {{"name", "Manual same ID"}, {"steam-id", "11"}},
                                  })}};
  platf::steam::game_t game;
  game.app_id = 10;
  game.name = "New";
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  ASSERT_EQ(root["apps"].size(), 2U);
  EXPECT_EQ(root["apps"][0]["name"], "New");
  EXPECT_EQ(root["apps"][1]["name"], "Manual same ID");
}

TEST(SteamSync, IdenticalManagedEntryDoesNotReportChange) {
  nlohmann::json root = {{"apps", nlohmann::json::array()}};
  platf::steam::game_t game;
  game.app_id = 10;
  game.name = "Same";
  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  EXPECT_FALSE(platf::steam::sync::policy::reconcile(root, {game}));
}

TEST(SteamSync, FiltersRuntimeByManifestTypeAndExplicitExclusion) {
  platf::steam::game_t game;
  game.app_id = 100;
  game.name = "Game";
  game.app_type = "game";
  platf::steam::game_t runtime;
  runtime.app_id = 101;
  runtime.name = "Runtime";
  runtime.app_type = "Tool";
  platf::steam::game_t excluded;
  excluded.app_id = 102;
  excluded.name = "Excluded";
  const std::vector<config::id_name_t> exclusions {{"102", "Excluded"}};
  const auto filtered = platf::steam::sync::policy::filter_games({game, runtime, excluded}, exclusions);
  ASSERT_EQ(filtered.size(), 1U);
  EXPECT_EQ(filtered[0].app_id, 100U);
}

TEST(SteamSync, FiltersCompatibilityNamesAndCurrentRuntimeIdsUnlessOptedIn) {
  platf::steam::game_t proton;
  proton.app_id = 5000000;
  proton.name = "Proton Future";
  platf::steam::game_t runtime;
  runtime.app_id = 4183110;
  runtime.name = "Steam Linux Runtime 4.0";
  platf::steam::game_t game;
  game.app_id = 42;
  game.name = "A Proton-Themed Game";

  const auto filtered = platf::steam::sync::policy::filter_games({proton, runtime, game}, {});
  ASSERT_EQ(filtered.size(), 1U);
  EXPECT_EQ(filtered[0].app_id, 42U);
  EXPECT_EQ(platf::steam::sync::policy::filter_games({proton, runtime, game}, {}, true).size(), 3U);
}

TEST(SteamSync, RemovesExcludedStaleAutoEntryButPreservesManualEntry) {
  nlohmann::json root = {{"apps", nlohmann::json::array({
    nlohmann::json {{"name", "Old auto"}, {"steam-id", "102"}, {"steam-managed", "auto"}},
    nlohmann::json {{"name", "Manual"}, {"steam-id", "102"}},
  })}};
  const std::vector<config::id_name_t> exclusions {{"102", "Excluded"}};
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {}, false, exclusions));
  ASSERT_EQ(root["apps"].size(), 1U);
  EXPECT_EQ(root["apps"][0]["name"], "Manual");
}

TEST(SteamSync, RecordsArtworkSourceWithoutPretendingJpegIsPng) {
  nlohmann::json root = {{"apps", nlohmann::json::array()}};
  platf::steam::game_t game;
  game.app_id = 103;
  game.name = "Art";
  game.portrait_path = "/tmp/library_600x900.jpg";
  game.artwork_path = game.portrait_path;
  game.artwork_format = "jpg";
  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  EXPECT_EQ(root["apps"][0]["steam-boxart-path"], "/tmp/library_600x900.jpg");
  EXPECT_FALSE(root["apps"][0].contains("steam-artwork-client-compatible"));
  EXPECT_EQ(root["apps"][0]["image-path"], "./assets/steam.png");
}

TEST(SteamSync, PublishesGeneratedClientArtworkAndPreservesSource) {
  nlohmann::json root = {{"apps", nlohmann::json::array()}};
  platf::steam::game_t game;
  game.app_id = 104;
  game.name = "Art";
  game.artwork_path = "/steam/library.jpg";
  game.artwork_format = "jpg";
  game.artwork_client_path = "/appdata/covers/steam_104.png";
  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  EXPECT_EQ(root["apps"][0]["steam-artwork-path"], "/steam/library.jpg");
  EXPECT_EQ(root["apps"][0]["steam-artwork-client-path"], "/appdata/covers/steam_104.png");
  EXPECT_TRUE(root["apps"][0]["steam-artwork-client-compatible"]);
  EXPECT_EQ(root["apps"][0]["image-path"], "/appdata/covers/steam_104.png");
}

TEST(SteamSync, FailedArtworkFallsBackWithoutReplacingCustomCover) {
  nlohmann::json root = {{"apps", nlohmann::json::array({nlohmann::json {
    {"steam-id", "105"}, {"steam-managed", "auto"}, {"image-path", "/custom/cover.png"},
    {"steam-artwork-client-path", "/appdata/covers/steam_105.png"}, {"steam-artwork-client-compatible", true}
  }})}};
  platf::steam::game_t game;
  game.app_id = 105;
  game.name = "Missing art";
  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  EXPECT_EQ(root["apps"][0]["image-path"], "/custom/cover.png");
  EXPECT_FALSE(root["apps"][0].contains("steam-artwork-client-path"));
  EXPECT_FALSE(root["apps"][0].contains("steam-artwork-client-compatible"));
}

TEST(SteamSync, MigratesLegacySteamUuidAndDeduplicatesManagedEntries) {
  nlohmann::json root = { {"apps", nlohmann::json::array({
    nlohmann::json {{"name", "Old copy"}, {"uuid", "STEAM-106"}, {"steam-managed", "auto"}},
    nlohmann::json {{"name", "Duplicate copy"}, {"steam-id", "106"}, {"uuid", "STEAM-106"}, {"steam-managed", "auto"}},
    nlohmann::json {{"name", "User shortcut"}, {"steam-id", "106"}},
  })} };
  platf::steam::game_t game;
  game.app_id = 106;
  game.name = "Current game";

  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  ASSERT_EQ(root["apps"].size(), 2U);
  EXPECT_EQ(root["apps"][0]["uuid"], platf::steam::sync::policy::canonical_steam_app_uuid(106));
  EXPECT_EQ(root["apps"][0]["steam-id"], "106");
  EXPECT_EQ(root["apps"][0]["name"], "Current game");
  EXPECT_EQ(root["apps"][1]["name"], "User shortcut");
  EXPECT_FALSE(platf::steam::sync::policy::reconcile(root, {game}));
}

TEST(SteamSync, ClearsStaleProviderMetadataButPreservesCustomCover) {
  nlohmann::json root = {{"apps", nlohmann::json::array({nlohmann::json {
    {"name", "Metadata game"}, {"steam-id", "107"}, {"steam-managed", "auto"},
    {"steam-app-type", "game"}, {"steam-install-dir", "/old/install"},
    {"steam-library-path", "/old/library"}, {"steam-icon-path", "/old/icon.png"},
    {"steam-header-path", "/old/header.jpg"}, {"steam-boxart-path", "/old/box.jpg"},
    {"steam-artwork-path", "/old/art.jpg"}, {"steam-artwork-format", "jpg"},
    {"steam-artwork-client-path", "/old/generated.png"}, {"steam-artwork-client-compatible", true},
    {"image-path", "/old/generated.png"}
  }})}};
  platf::steam::game_t game;
  game.app_id = 107;
  game.name = "Metadata game";

  ASSERT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  const auto &app = root["apps"][0];
  for (const auto *key : {"steam-app-type", "steam-install-dir", "steam-library-path", "steam-icon-path",
                           "steam-header-path", "steam-boxart-path", "steam-artwork-path", "steam-artwork-format",
                           "steam-artwork-client-path", "steam-artwork-client-compatible"}) {
    EXPECT_FALSE(app.contains(key)) << key;
  }
  EXPECT_EQ(app["image-path"], "./assets/steam.png");

  root["apps"][0]["image-path"] = "/user/cover.png";
  root["apps"][0]["steam-artwork-client-path"] = "/old/generated.png";
  root["apps"][0]["steam-artwork-client-compatible"] = true;
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {game}));
  EXPECT_EQ(root["apps"][0]["image-path"], "/user/cover.png");
  EXPECT_FALSE(root["apps"][0].contains("steam-artwork-client-path"));
}

TEST(SteamSync, ExclusionNamesAreCaseInsensitiveForDiscoveryAndStaleEntries) {
  platf::steam::game_t game;
  game.app_id = 108;
  game.name = "Case Sensitive Name";
  const std::vector<config::id_name_t> exclusions {{"", "case sensitive name"}};
  EXPECT_TRUE(platf::steam::sync::policy::filter_games({game}, exclusions).empty());

  nlohmann::json root = { {"apps", nlohmann::json::array({nlohmann::json {
    {"name", "STALE NAME"}, {"steam-id", "109"}, {"steam-managed", "auto"}
  }})} };
  const std::vector<config::id_name_t> stale_exclusion {{"", "stale name"}};
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {}, false, stale_exclusion));
  EXPECT_TRUE(root["apps"].empty());
}

TEST(SteamSync, KeepsOneDuplicateStaleEntryWhenRemovalDisabled) {
  nlohmann::json root = { {"apps", nlohmann::json::array({
    nlohmann::json {{"name", "Stale one"}, {"steam-id", "110"}, {"steam-managed", "auto"}},
    nlohmann::json {{"name", "Stale two"}, {"steam-id", "110"}, {"steam-managed", "auto"}},
  })} };
  EXPECT_TRUE(platf::steam::sync::policy::reconcile(root, {}, false));
  ASSERT_EQ(root["apps"].size(), 1U);
  EXPECT_EQ(root["apps"][0]["steam-id"], "110");
  EXPECT_FALSE(platf::steam::sync::policy::reconcile(root, {}, false));
}

#include "src/lutris_integration.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>

namespace fs = std::filesystem;

TEST(LutrisDiscovery, ReadsInstalledGamesAndClassifiesSteam) {
  const auto base = fs::temp_directory_path() /
                    ("vibeshine-lutris-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(base);
  const auto path = base / "pga.db";
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  const char *schema =
    "CREATE TABLE games(id INTEGER, name TEXT, slug TEXT, runner TEXT, platform TEXT, directory TEXT, "
    "configpath TEXT, service TEXT, service_id TEXT, lastplayed INTEGER, playtime REAL, installed INTEGER);"
    "INSERT INTO games VALUES(1,'Battle.net','battlenet','wine','Windows','/games/battlenet','battlenet','', '',10,12.5,1);"
    "INSERT INTO games VALUES(2,'Steam Game','steam-game','steam','Linux','', 'steam-game','steam','42',20,2.0,1);"
    "INSERT INTO games VALUES(3,'Removed','removed','linux','Linux','/games/removed','removed','','',0,0,0);";
  ASSERT_EQ(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(database);

  const auto games = platf::lutris::discover(path);
  ASSERT_EQ(games.size(), 2U);
  EXPECT_EQ(games[0].stable_id, "lutris:1");
  EXPECT_FALSE(games[0].steam_backed());
  EXPECT_EQ(games[1].stable_id, "steam:42");
  EXPECT_TRUE(games[1].steam_backed());
  EXPECT_NE(platf::lutris::source_fingerprint(games), 0U);
  fs::remove_all(base);
}

TEST(LutrisLaunch, BuildsValidatedUri) {
  EXPECT_EQ(platf::lutris::launch_uri(7), "lutris:rungameid/7");
  EXPECT_TRUE(platf::lutris::launch_uri(0).empty());
  EXPECT_FALSE(platf::lutris::launch(0));
}

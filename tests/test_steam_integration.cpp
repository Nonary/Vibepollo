#include "src/steam_integration.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace fs = std::filesystem;
using namespace platf::steam;

TEST(SteamVdf, ParsesNestedEscapesAndComments) {
  const auto doc = parse_vdf(R"VDF(
    // comment
    "AppState" { "appid" "123" "name" "A \"Game\"" "apps" { "123" "1" } }
  )VDF");
  ASSERT_NE(doc.find("AppState"), nullptr);
  EXPECT_EQ(doc.find("AppState")->find("appid")->value, "123");
  EXPECT_EQ(doc.find("AppState")->find("name")->value, "A \"Game\"");
}

TEST(SteamDiscovery, ReadsManifestsAndLibraryFolders) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "steamapps", ec);
  fs::create_directories(base / "library" / "steamapps" / "common" / "Example", ec);
  {
    std::ofstream out(base / "steamapps/libraryfolders.vdf");
    out << R"VDF("libraryfolders" { "0" { "path" ")VDF" << (base / "library").string() << R"VDF(" } })VDF";
  }
  {
    std::ofstream out(base / "library/steamapps/appmanifest_42.acf");
    out << R"VDF("AppState" { "appid" "42" "name" "Example" "installdir" "Example" "StateFlags" "4" })VDF";
  }
  const auto games = discover({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].stable_id, "steam:42");
  EXPECT_EQ(games[0].name, "Example");
  EXPECT_EQ(games[0].install_dir.filename(), "Example");
  fs::remove_all(base, ec);
}

TEST(SteamDiscovery, FindsModernCentralPortraitForExternalLibrary) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-art-test-" + std::to_string(nonce));
  const auto library = base / "external";
  std::error_code ec;
  fs::create_directories(base / "steamapps", ec);
  fs::create_directories(library / "steamapps" / "common" / "CoverGame", ec);
  fs::create_directories(base / "appcache/librarycache/42", ec);
  fs::create_directories(base / "userdata/123/config/grid", ec);
  {
    std::ofstream out(base / "steamapps/libraryfolders.vdf");
    out << R"VDF("libraryfolders" { "0" { "path" ")VDF" << library.string() << R"VDF(" } })VDF";
  }
  {
    std::ofstream out(library / "steamapps/appmanifest_42.acf");
    out << R"VDF("AppState" { "appid" "42" "name" "Cover Game" "type" "game" "installdir" "CoverGame" })VDF";
  }
  std::ofstream(base / "appcache/librarycache/42/library_600x900.jpg") << "jpg";
  std::ofstream(base / "appcache/librarycache/42_library_600x900_2x.jpg") << "2x";
  std::ofstream(base / "appcache/librarycache/42/header.jpg") << "jpg";
  std::ofstream(base / "userdata/123/config/grid/42p.png") << "png";

  const auto games = discover({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].portrait_path.filename(), "42_library_600x900_2x.jpg");
  EXPECT_EQ(games[0].artwork_path, games[0].portrait_path);
  EXPECT_EQ(games[0].artwork_format, "jpg");
  EXPECT_EQ(games[0].header_path.filename(), "header.jpg");
  fs::remove_all(base, ec);
}

TEST(SteamDiscovery, IgnoresManifestWithoutInstalledDirectory) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-stale-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::create_directories(base / "steamapps", ec);
  {
    std::ofstream out(base / "steamapps/appmanifest_84.acf");
    out << R"VDF("AppState" { "appid" "84" "name" "Removed Game" "installdir" "Missing" "StateFlags" "4" })VDF";
  }
  EXPECT_TRUE(discover({base}).empty());
  fs::remove_all(base, ec);
}

TEST(SteamLaunch, RejectsZeroAndBuildsValidatedUri) {
  EXPECT_TRUE(launch_uri(480).ends_with("steam://rungameid/480"));
  EXPECT_TRUE(launch_uri(0).empty());
  EXPECT_TRUE(launch_command(0).empty());
#ifdef _WIN32
  EXPECT_EQ(launch_command(480), "cmd /c start \"\" steam://rungameid/480");
#elif defined(__APPLE__)
  EXPECT_EQ(launch_command(480), "open steam://rungameid/480");
#else
  EXPECT_EQ(launch_command(480), "steam -applaunch 480");
#endif
  EXPECT_FALSE(launch(0));
}

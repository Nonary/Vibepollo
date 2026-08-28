#include "src/steam_integration.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using namespace platf::steam;

namespace {
  void append_u32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::uint8_t>(value >> shift));
  }

  void append_u64(std::vector<std::uint8_t> &data, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) data.push_back(static_cast<std::uint8_t>(value >> shift));
  }

  void append_string(std::vector<std::uint8_t> &data, std::string_view value) {
    data.insert(data.end(), value.begin(), value.end());
    data.push_back(0);
  }

  void append_object(std::vector<std::uint8_t> &data, std::uint32_t key) {
    data.push_back(0x00);
    append_u32(data, key);
  }

  void append_value(std::vector<std::uint8_t> &data, std::uint32_t key, std::string_view value) {
    data.push_back(0x01);
    append_u32(data, key);
    append_string(data, value);
  }

  void write_test_appinfo(const fs::path &path, std::uint32_t app_id) {
    const std::vector<std::string> keys {
      "appinfo", "config", "launch", "0", "executable", "arguments", "workingdir", "type", "oslist"
    };
    std::vector<std::uint8_t> blob;
    append_object(blob, 0);
    append_object(blob, 1);
    append_object(blob, 2);
    append_object(blob, 3);
    append_value(blob, 4, "Game.exe");
    append_value(blob, 5, "-from-appinfo");
    append_value(blob, 6, "bin");
    append_value(blob, 7, "default");
    append_object(blob, 1);
    append_value(blob, 8, "windows");
    blob.insert(blob.end(), {0x08, 0x08, 0x08, 0x08, 0x08, 0x08});

    std::vector<std::uint8_t> data;
    append_u32(data, 0x07564429);
    append_u32(data, 1);
    append_u64(data, 0);  // Patched after the entry is assembled.
    append_u32(data, app_id);
    append_u32(data, static_cast<std::uint32_t>(60 + blob.size()));
    data.resize(data.size() + 60, 0);
    data.insert(data.end(), blob.begin(), blob.end());
    append_u32(data, 0);  // App-entry sentinel.
    const auto table_offset = data.size();
    for (unsigned shift = 0; shift < 64; shift += 8) data[8 + shift / 8] = static_cast<std::uint8_t>(table_offset >> shift);
    append_u32(data, static_cast<std::uint32_t>(keys.size()));
    for (const auto &key : keys) append_string(data, key);

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  }
}

TEST(SteamVdf, ParsesNestedEscapesAndComments) {
  const auto doc = parse_vdf(R"VDF(
    // comment
    "AppState" { "appid" "123" "name" "A \"Game\"" "apps" { "123" "1" } }
  )VDF");
  ASSERT_NE(doc.find("AppState"), nullptr);
  EXPECT_EQ(doc.find("AppState")->find("appid")->value, "123");
  EXPECT_EQ(doc.find("AppState")->find("name")->value, "A \"Game\"");
}

TEST(SteamVdf, IgnoresStrayRootBracesWithoutStalling) {
  const auto doc = parse_vdf(R"VDF("root" { "value" "ok" } })VDF");
  ASSERT_NE(doc.find("root"), nullptr);
  EXPECT_EQ(doc.find("root")->find("value")->value, "ok");
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

TEST(SteamDiscovery, FindsContentHashedLibraryCapsule) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-hashed-art-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::create_directories(base / "steamapps/common/HashedCoverGame", ec);
  fs::create_directories(base / "appcache/librarycache/84/capsule-hash", ec);
  fs::create_directories(base / "appcache/librarycache/84/header-hash", ec);
  {
    std::ofstream out(base / "steamapps/appmanifest_84.acf");
    out << R"VDF("AppState" { "appid" "84" "name" "Hashed Cover Game" "type" "game" "installdir" "HashedCoverGame" })VDF";
  }
  std::ofstream(base / "appcache/librarycache/84/capsule-hash/library_capsule.jpg") << "capsule";
  std::ofstream(base / "appcache/librarycache/84/header-hash/library_header.jpg") << "header";

  const auto games = discover({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].portrait_path.filename(), "library_capsule.jpg");
  EXPECT_EQ(games[0].artwork_path, games[0].portrait_path);
  EXPECT_EQ(games[0].artwork_format, "jpg");
  EXPECT_EQ(games[0].header_path.filename(), "library_header.jpg");
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

#ifdef __linux__
TEST(SteamDiscovery, ResolvesDirectLaunchMetadataOptionsAndExistingProton) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-direct-test-" + std::to_string(nonce));
  const auto install = base / "steamapps/common/DirectGame";
  const auto compatdata = base / "steamapps/compatdata/42";
  const auto proton = base / "compatibilitytools.d/TestProton";
  std::error_code ec;
  fs::create_directories(install / "bin", ec);
  fs::create_directories(compatdata, ec);
  fs::create_directories(base / "appcache", ec);
  fs::create_directories(base / "userdata/123/config", ec);
  {
    std::ofstream output(base / "steamapps/appmanifest_42.acf");
    output << R"VDF("AppState" { "appid" "42" "name" "Direct Game" "installdir" "DirectGame" })VDF";
  }
  write_test_appinfo(base / "appcache/appinfo.vdf", 42);
  {
    std::ofstream output(compatdata / "config_info");
    output << "Test Proton\n" << (proton / "files/share/fonts").string() << "/\n";
  }
  {
    std::ofstream output(base / "userdata/123/config/localconfig.vdf");
    output << R"VDF("UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" { "42" {
      "LaunchOptions" "MANGOHUD_CONFIG=fps_limit=30 mangohud %command% -user"
    } } } } } })VDF";
  }

  const auto games = discover({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].launch_executable, install / "Game.exe");
  EXPECT_EQ(games[0].launch_working_dir, install / "bin");
  EXPECT_EQ(games[0].launch_arguments, "-from-appinfo");
  EXPECT_EQ(games[0].launch_options, "MANGOHUD_CONFIG=fps_limit=30 mangohud %command% -user");
  EXPECT_EQ(games[0].launch_os, "windows");
  EXPECT_EQ(games[0].compatdata_path, compatdata);
  EXPECT_EQ(games[0].proton_path, proton);
  EXPECT_NE(launch_command(games[0]).find("mangohud vibepollo-mangohud --appid 42 -- env"), std::string::npos);
  fs::remove_all(base, ec);
}
#endif

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

#ifdef __linux__
TEST(SteamLaunch, DirectLaunchPlacesVibepolloInsideInheritedSteamOptions) {
  game_t game;
  game.app_id = 1182900;
  game.launch_executable = "/games/A Plague Tale/APlagueTaleRequiem_x64.exe";
  game.launch_working_dir = game.launch_executable.parent_path();
  game.launch_os = "windows";
  game.compatdata_path = "/games/steamapps/compatdata/1182900";
  game.proton_path = "/steam/compatibilitytools.d/GE-Proton11-5";
  game.launch_options = "PROTON_DLSS_UPGRADE=3.7 mangohud %command% -windowed";

  EXPECT_EQ(
    launch_command(game),
    "PROTON_DLSS_UPGRADE=3.7 mangohud "
    "vibepollo-mangohud --appid 1182900 -- env SteamAppId=1182900 SteamGameId=1182900 "
    "GAMEID=umu-1182900 STORE=steam "
    "WINEPREFIX='/games/steamapps/compatdata/1182900' "
    "PROTONPATH='/steam/compatibilitytools.d/GE-Proton11-5' "
    "umu-run '/games/A Plague Tale/APlagueTaleRequiem_x64.exe' -windowed"
  );
}

TEST(SteamLaunch, DirectNativeLaunchTreatsOptionsWithoutPlaceholderAsArguments) {
  game_t game;
  game.app_id = 480;
  game.launch_executable = "/games/Spacewar/spacewar";
  game.launch_os = "linux";
  game.launch_arguments = "-default";
  game.launch_options = "-user-option";

  EXPECT_EQ(
    launch_command(game),
    "vibepollo-mangohud --appid 480 -- env SteamAppId=480 SteamGameId=480 "
    "'/games/Spacewar/spacewar' -default -user-option"
  );
}

TEST(SteamLaunch, FallsBackToBrokerWhenProtonMetadataIsUnavailable) {
  game_t game;
  game.app_id = 480;
  game.launch_executable = "/games/Spacewar/spacewar.exe";
  game.launch_os = "windows";
  EXPECT_EQ(launch_command(game), "steam -applaunch 480");
}
#endif

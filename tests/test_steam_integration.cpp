#include "src/steam_integration.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <vector>

#if defined(__linux__)
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace platf::steam;

namespace {
#if defined(__linux__)
  class scoped_environment final {
  public:
    scoped_environment(const char *name, const std::string &value): name_ {name} {
      if (const auto *current = std::getenv(name)) {
        previous_ = current;
      }
      setenv(name, value.c_str(), 1);
    }

    ~scoped_environment() {
      if (previous_) {
        setenv(name_.c_str(), previous_->c_str(), 1);
      } else {
        unsetenv(name_.c_str());
      }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
  };
#endif

  void append_u32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      data.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void append_u64(std::vector<std::uint8_t> &data, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      data.push_back(static_cast<std::uint8_t>(value >> shift));
    }
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

  void write_test_appinfo(const fs::path &path, std::uint32_t app_id, std::string_view name = "Example") {
    const std::vector<std::string> keys {
      "appinfo",
      "config",
      "launch",
      "0",
      "executable",
      "arguments",
      "workingdir",
      "type",
      "oslist",
      "common",
      "name"
    };
    std::vector<std::uint8_t> blob;
    append_object(blob, 0);
    append_object(blob, 9);
    append_value(blob, 10, name);
    append_value(blob, 7, "game");
    blob.push_back(0x08);
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
    for (unsigned shift = 0; shift < 64; shift += 8) {
      data[8 + shift / 8] = static_cast<std::uint8_t>(table_offset >> shift);
    }
    append_u32(data, static_cast<std::uint32_t>(keys.size()));
    for (const auto &key : keys) {
      append_string(data, key);
    }

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  }
}  // namespace

#if defined(__linux__)
TEST(SteamDiscovery, MachineHostDoesNotParseSessionHome) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-session-home-test-" + std::to_string(nonce));
  const auto session_home = base / "desktop";
  const auto service_home = base / "machine";
  std::error_code ec;
  fs::create_directories(session_home / ".local/share/Steam", ec);
  fs::create_directories(service_home / ".local/share/Steam", ec);
  scoped_environment machine {"VIBEPOLLO_MACHINE_HOST", "1"};
  scoped_environment session {"VIBEPOLLO_SESSION_HOME", session_home.string()};
  scoped_environment home {"HOME", service_home.string()};
  scoped_environment xdg {"XDG_DATA_HOME", (service_home / ".local/share").string()};

  const auto roots = default_library_roots();
  EXPECT_EQ(std::find(roots.begin(), roots.end(), fs::weakly_canonical(session_home / ".local/share/Steam")), roots.end());
  EXPECT_EQ(std::find(roots.begin(), roots.end(), fs::weakly_canonical(service_home / ".local/share/Steam")), roots.end());
  EXPECT_TRUE(roots.empty());
  fs::remove_all(base, ec);
}
#endif

TEST(SteamDiscovery, CatalogIncludesPlayedUninstalledGamesWithNames) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-catalog-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::create_directories(base / "steamapps", ec);
  fs::create_directories(base / "appcache", ec);
  fs::create_directories(base / "userdata/123/config", ec);
  {
    std::ofstream out(base / "steamapps/libraryfolders.vdf");
    out << R"VDF("libraryfolders" { "0" { "path" ")VDF" << base.string() << R"VDF(" } })VDF";
  }
  {
    std::ofstream out(base / "userdata/123/config/localconfig.vdf");
    out << R"VDF("UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" {
      "4242" { "LastPlayed" "1700000000" "Playtime" "321" }
    } } } } })VDF";
  }
  write_test_appinfo(base / "appcache/appinfo.vdf", 4242, "History Game");

  const auto games = discover_catalog({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].app_id, 4242U);
  EXPECT_EQ(games[0].name, "History Game");
  EXPECT_FALSE(games[0].installed);
  EXPECT_EQ(games[0].last_played, 1700000000U);
  EXPECT_EQ(games[0].playtime_minutes, 321U);
  fs::remove_all(base, ec);
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
  const auto proton = base / "steamapps/common/TestProton";
  const auto runtime = base / "steamapps/common/TestRuntime";
  std::error_code ec;
  fs::create_directories(install / "bin", ec);
  fs::create_directories(compatdata, ec);
  fs::create_directories(proton, ec);
  fs::create_directories(runtime, ec);
  fs::create_directories(base / "appcache", ec);
  fs::create_directories(base / "userdata/123/config", ec);
  {
    std::ofstream output(base / "steamapps/appmanifest_42.acf");
    output << R"VDF("AppState" { "appid" "42" "name" "Direct Game" "installdir" "DirectGame" })VDF";
  }
  write_test_appinfo(base / "appcache/appinfo.vdf", 42);
  {
    std::ofstream output(compatdata / "config_info");
    output << "Test Proton\n"
           << (proton / "files/share/fonts").string() << "/\n";
  }
  {
    std::ofstream output(proton / "toolmanifest.vdf");
    output << R"VDF("manifest" { "require_tool_appid" "99" })VDF";
  }
  {
    std::ofstream output(base / "steamapps/appmanifest_99.acf");
    output << R"VDF("AppState" { "appid" "99" "installdir" "TestRuntime" })VDF";
  }
  std::ofstream(runtime / "_v2-entry-point") << "runtime";
  {
    std::ofstream output(base / "userdata/123/config/localconfig.vdf");
    output << R"VDF("UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" { "42" {
      "LaunchOptions" "MANGOHUD_CONFIG=fps_limit=30 mangohud %command% -user"
    } } } } } })VDF";
  }

  const auto games = discover({base});
  ASSERT_GE(games.size(), 1U);
  ASSERT_EQ(games[0].app_id, 42U);
  EXPECT_EQ(games[0].launch_executable, install / "Game.exe");
  EXPECT_EQ(games[0].launch_working_dir, install / "bin");
  EXPECT_EQ(games[0].launch_arguments, "-from-appinfo");
  EXPECT_EQ(games[0].launch_options, "MANGOHUD_CONFIG=fps_limit=30 mangohud %command% -user");
  EXPECT_EQ(games[0].launch_os, "windows");
  EXPECT_EQ(games[0].compatdata_path, compatdata);
  EXPECT_EQ(games[0].proton_path, proton);
  EXPECT_EQ(games[0].proton_runtime_path, runtime);
  EXPECT_EQ(games[0].steam_client_path, base);
  EXPECT_NE(launch_command(games[0]).find("mangohud /usr/bin/vibepollo-mangohud --appid 42 -- env"), std::string::npos);

  // Custom compatibility tools live outside steamapps. Modern config_info
  // records the Steam client root explicitly, which must be used to resolve
  // the required pressure-vessel runtime.
  const auto client = base / "client";
  const auto external_proton = client / "compatibilitytools.d/ExternalProton";
  fs::create_directories(external_proton / "files/share/fonts", ec);
  fs::create_directories(client / "steamapps", ec);
  {
    std::ofstream output(external_proton / "toolmanifest.vdf");
    output << R"VDF("manifest" { "require_tool_appid" "99" })VDF";
  }
  {
    std::ofstream output(compatdata / "config_info");
    output << "External Proton\n"
           << (external_proton / "files/share/fonts").string() << "/\n"
           << (external_proton / "files/lib").string() << "/\n"
           << client.string() << "\n";
  }
  const auto custom_tool_games = discover({base});
  ASSERT_GE(custom_tool_games.size(), 1U);
  EXPECT_EQ(custom_tool_games[0].proton_path, external_proton);
  EXPECT_EQ(custom_tool_games[0].steam_client_path, client);
  EXPECT_EQ(custom_tool_games[0].proton_runtime_path, runtime);
  EXPECT_NE(launch_command(custom_tool_games[0]).find("ExternalProton/proton"), std::string::npos);

  // Compatibility tools can also be installed outside a Steam library. The
  // upward search must stop at the filesystem root and use the broker fallback
  // when older config_info metadata does not identify the Steam client.
  {
    std::ofstream output(compatdata / "config_info");
    output << "External Proton\n/opt/proton/files/share/fonts/\n";
  }
  const auto external_games = discover({base});
  ASSERT_GE(external_games.size(), 1U);
  EXPECT_TRUE(external_games[0].proton_runtime_path.empty());
  EXPECT_EQ(launch_command(external_games[0]), "steam -applaunch 42");
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

TEST(SteamLaunch, StreamOwnedEnvironmentFeaturesRequireDirectLaunch) {
  EXPECT_TRUE(requires_direct_environment_launch(true, false));
  EXPECT_TRUE(requires_direct_environment_launch(false, true));
  EXPECT_TRUE(requires_direct_environment_launch(true, true));
  EXPECT_FALSE(requires_direct_environment_launch(false, false));
}

TEST(SteamLaunch, GamingModeReplacesCachedDesktopProtonCommand) {
  const std::string cached = "/bin/sh -c 'old-release/vibepollo-mangohud --appid 1145350 -- proton waitforexitandrun Hades2.exe'";
  EXPECT_EQ(runtime_launch_command("1145350", cached, true), launch_command(1145350));
  EXPECT_EQ(runtime_launch_command("1145350", cached, false), cached);
  EXPECT_EQ(runtime_launch_command("", "custom-game", true), "custom-game");
  for (const auto invalid : {"0", "-1", "1145350; touch /tmp/unwanted", "4294967296"}) {
    EXPECT_TRUE(runtime_launch_command(invalid, cached, true).empty());
  }
}

#ifdef __linux__
TEST(SteamLaunch, MachineClientFallbackUsesCanonicalSemanticArguments) {
  const std::string prefix = "/usr/libexec/vibeshine/vibepollo-session-exec steam ";
  const auto arguments = session_launch_arguments(prefix + "1182900");
  ASSERT_TRUE(arguments);
  EXPECT_EQ(*arguments, (std::vector<std::string> {"steam", "1182900"}));
  for (const auto suffix : {"0", "-1", "4294967296", "01182900", "1182900 ",
                            "1182900 trailing", "1182900;id", "1182900\n", ""}) {
    EXPECT_FALSE(session_launch_arguments(prefix + suffix)) << suffix;
  }
  EXPECT_FALSE(session_launch_arguments("/tmp/vibepollo-session-exec steam 1182900"));
}

TEST(SteamLaunch, MachineSessionLaunchUsesCanonicalSemanticArguments) {
  session_launch_policy_t policy {
    .provider = "mangohud-proton",
    .limit_millihz = 116000,
    .preset = "3",
    .always_show_graph = true,
    .limiter_method = "late",
    .smooth_motion = true,
    .smooth_motion_graphics_queue = true,
  };
  const auto command = session_launch_command(1182900, policy);
  EXPECT_EQ(
    command,
    "/usr/libexec/vibeshine/vibepollo-session-exec steam-direct "
    "1182900 mangohud-proton 116000 3 1 late 1 1"
  );
  const auto arguments = session_launch_arguments(command);
  ASSERT_TRUE(arguments);
  EXPECT_EQ(
    *arguments,
    (std::vector<std::string> {
      "steam-direct", "1182900", "mangohud-proton", "116000", "3",
      "1", "late", "1", "1"
    })
  );

  policy.provider = "disabled";
  EXPECT_TRUE(session_launch_command(1182900, policy).empty());
  EXPECT_FALSE(session_launch_arguments(command + " trailing"));
  EXPECT_FALSE(session_launch_arguments(
    "/usr/libexec/vibeshine/vibepollo-session-exec steam-direct "
    "1182900 proton 116000 custom 1 late 0 0"
  ));
}

TEST(SteamLaunch, DirectLaunchPlacesVibepolloInsideInheritedSteamOptions) {
  game_t game;
  game.app_id = 1182900;
  game.launch_executable = "/games/A Plague Tale/APlagueTaleRequiem_x64.exe";
  game.launch_working_dir = game.launch_executable.parent_path();
  game.launch_os = "windows";
  game.install_dir = "/games/A Plague Tale";
  game.library_path = "/games";
  game.compatdata_path = "/games/steamapps/compatdata/1182900";
  game.proton_path = "/steam/compatibilitytools.d/GE-Proton11-5";
  game.proton_runtime_path = "/steam/steamapps/common/SteamLinuxRuntime_4";
  game.steam_client_path = "/steam";
  game.launch_options = "PROTON_DLSS_UPGRADE=3.7 mangohud %command% -windowed";

  const auto command = launch_command(game);
  EXPECT_TRUE(command.starts_with("/bin/sh -c 'PROTON_DLSS_UPGRADE=3.7 mangohud "));
  EXPECT_NE(command.find("/usr/bin/vibepollo-mangohud --appid 1182900 -- env"), std::string::npos);
  EXPECT_NE(command.find("STEAM_COMPAT_APP_ID=1182900"), std::string::npos);
  EXPECT_NE(command.find("STEAM_COMPAT_SHADER_PATH="), std::string::npos);
  EXPECT_NE(command.find("STEAM_COMPAT_MEDIA_PATH="), std::string::npos);
  EXPECT_NE(command.find("STEAM_COMPAT_TRANSCODED_MEDIA_PATH="), std::string::npos);
  EXPECT_NE(command.find("__GL_SHADER_DISK_CACHE_PATH="), std::string::npos);
  EXPECT_NE(command.find("SteamLinuxRuntime_4/_v2-entry-point"), std::string::npos);
  EXPECT_NE(command.find("GE-Proton11-5/proton"), std::string::npos);
  EXPECT_EQ(command.find("umu-run"), std::string::npos);
  EXPECT_TRUE(command.ends_with("-windowed'"));
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
    "/bin/sh -c '/usr/bin/vibepollo-mangohud --appid 480 -- env SteamAppId=480 SteamGameId=480 "
    "'\\''/games/Spacewar/spacewar'\\'' -default -user-option'"
  );
}

TEST(SteamLaunch, RelocatedBundleExecutesItsOwnHelperWithQuotedPath) {
  constexpr auto fixture_variable = "VIBESHINE_TEST_RELOCATED_STEAM_HELPER";
  if (const auto *fixture = std::getenv(fixture_variable)) {
    game_t game;
    game.app_id = 480;
    game.launch_executable = "/usr/bin/true";
    game.launch_os = "linux";
    // The generated command must locate its helper independently of cwd/PATH,
    // preserve the apostrophe in the bundle directory, and execute the game.
    fs::current_path("/");
    const auto command = launch_command(game);
    EXPECT_EQ(std::system(command.c_str()), 0);
    EXPECT_TRUE(fs::is_regular_file(fs::path(fixture) / "helper-ran"));
    return;
  }

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto bundle = fs::temp_directory_path() / ("vibepollo Steam's bundle " + std::to_string(nonce));
  struct fixture_cleanup {
    fs::path path;
    ~fixture_cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup {bundle};
  fs::create_directories(bundle);
  const auto host = bundle / "vibepollo-test";
  fs::copy_file(fs::read_symlink("/proc/self/exe"), host);
  const auto helper = bundle / "vibepollo-mangohud";
  {
    std::ofstream output(helper);
    output << "#!/bin/sh\nset -eu\n"
              "[ \"$1\" = --appid ] && [ \"$2\" = 480 ] && [ \"$3\" = -- ]\n"
              "shift 3\n"
              "touch -- \"${0%/*}/helper-ran\"\n"
              "exec \"$@\"\n";
  }
  fs::permissions(helper, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);

  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    setenv(fixture_variable, bundle.c_str(), 1);
    execl(host.c_str(), host.c_str(),
          "--gtest_filter=SteamLaunch.RelocatedBundleExecutesItsOwnHelperWithQuotedPath",
          static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_TRUE(fs::is_regular_file(bundle / "helper-ran"));
}

TEST(SteamLaunch, FallsBackToBrokerWhenProtonMetadataIsUnavailable) {
  game_t game;
  game.app_id = 480;
  game.launch_executable = "/games/Spacewar/spacewar.exe";
  game.launch_os = "windows";
  EXPECT_EQ(launch_command(game), "steam -applaunch 480");
}

TEST(SteamLaunch, FallsBackToBrokerWhenRequiredRuntimeIsUnavailable) {
  game_t game;
  game.app_id = 480;
  game.launch_executable = "/games/Spacewar/spacewar.exe";
  game.launch_os = "windows";
  game.compatdata_path = "/games/steamapps/compatdata/480";
  game.proton_path = "/steam/steamapps/common/Proton";
  EXPECT_EQ(launch_command(game), "steam -applaunch 480");
}
#endif

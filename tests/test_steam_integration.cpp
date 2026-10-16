#include "src/steam_integration.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
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

  void write_test_appinfo(const fs::path &path, std::uint32_t app_id, std::string_view name = "Example", bool include_linux = false, bool include_windows = true, bool include_official_tool = false) {
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
      "name",
      "1",
      "extended",
      "compat_tools",
      "proton_fixture",
      "appid",
      "from_oslist",
      "to_oslist",
      "aliases"
    };
    std::vector<std::uint8_t> blob;
    append_object(blob, 0);
    append_object(blob, 9);
    append_value(blob, 10, name);
    append_value(blob, 7, "game");
    blob.push_back(0x08);
    append_object(blob, 1);
    append_object(blob, 2);
    if (include_windows) {
      append_object(blob, 3);
      append_value(blob, 4, "Game.exe");
      append_value(blob, 5, "-from-appinfo");
      append_value(blob, 6, "bin");
      append_value(blob, 7, "default");
      append_object(blob, 1);
      append_value(blob, 8, "windows");
      blob.insert(blob.end(), {0x08, 0x08});
    }
    if (include_linux) {
      append_object(blob, 11);
      append_value(blob, 4, "Game.sh");
      append_value(blob, 5, "-native-from-appinfo");
      append_value(blob, 7, "default");
      append_object(blob, 1);
      append_value(blob, 8, "linux");
      blob.insert(blob.end(), {0x08, 0x08});
    }
    blob.insert(blob.end(), {0x08, 0x08, 0x08, 0x08});

    std::vector<std::uint8_t> data;
    append_u32(data, 0x07564429);
    append_u32(data, 1);
    append_u64(data, 0);  // Patched after the entry is assembled.
    append_u32(data, app_id);
    append_u32(data, static_cast<std::uint32_t>(60 + blob.size()));
    data.resize(data.size() + 60, 0);
    data.insert(data.end(), blob.begin(), blob.end());
    if (include_official_tool) {
      std::vector<std::uint8_t> manifest;
      append_object(manifest, 0);
      append_object(manifest, 12);
      append_object(manifest, 13);
      append_object(manifest, 14);
      append_value(manifest, 15, "199");
      append_value(manifest, 16, "windows");
      append_value(manifest, 17, "linux");
      append_value(manifest, 18, "proton-fixture-alias");
      manifest.insert(manifest.end(), {0x08, 0x08, 0x08, 0x08, 0x08});
      append_u32(data, 891390);
      append_u32(data, static_cast<std::uint32_t>(60 + manifest.size()));
      data.resize(data.size() + 60, 0);
      data.insert(data.end(), manifest.begin(), manifest.end());
    }
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
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-catalog-test-" + std::to_string(nonce));
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
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "steamapps", ec);
  fs::create_directories(base / "library" / "steamapps" / "common" / "Example", ec);
  {
    std::ofstream out(base / "steamapps/libraryfolders.vdf");
    out << R"VDF("libraryfolders" { "0" { "path" ")VDF" << (base / "library").generic_string() << R"VDF(" } })VDF";
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
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-art-test-" + std::to_string(nonce));
  const auto library = base / "external";
  std::error_code ec;
  fs::create_directories(base / "steamapps", ec);
  fs::create_directories(library / "steamapps" / "common" / "CoverGame", ec);
  fs::create_directories(base / "appcache/librarycache/42", ec);
  fs::create_directories(base / "userdata/123/config/grid", ec);
  {
    std::ofstream out(base / "steamapps/libraryfolders.vdf");
    out << R"VDF("libraryfolders" { "0" { "path" ")VDF" << library.generic_string() << R"VDF(" } })VDF";
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

TEST(SteamDiscovery, FindsContentHashedLibrary600x900) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-hashed-600x900-test-" + std::to_string(nonce));
  std::error_code ec;
  fs::create_directories(base / "steamapps/common/NewCoverGame", ec);
  fs::create_directories(base / "appcache/librarycache/85/portrait-hash", ec);
  fs::create_directories(base / "appcache/librarycache/85/header-hash", ec);
  {
    std::ofstream out(base / "steamapps/appmanifest_85.acf");
    out << R"VDF("AppState" { "appid" "85" "name" "New Cover Game" "type" "game" "installdir" "NewCoverGame" })VDF";
  }
  std::ofstream(base / "appcache/librarycache/85/portrait-hash/library_600x900.jpg") << "portrait";
  std::ofstream(base / "appcache/librarycache/85/header-hash/library_header.jpg") << "header";

  const auto games = discover({base});
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].portrait_path.filename(), "library_600x900.jpg");
  EXPECT_EQ(games[0].artwork_path, games[0].portrait_path);
  fs::remove_all(base, ec);
}

TEST(SteamDiscovery, IgnoresManifestWithoutInstalledDirectory) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
  const auto base = fs::temp_directory_path() / ("vibepollo-steam-stale-test-" + std::to_string(nonce));
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
namespace {
  // Synthetic installation using Steam's on-disk schemas, not a copy of a
  // user's files. Mixed-platform appinfo plus only the Windows payload models
  // Nonary/vibeshine#294. Keep the client, game and runtime in separate roots.
  class SteamCompatibilityOverrides: public ::testing::Test {
  protected:
    static constexpr std::uint32_t app_id = 588650;
    scoped_environment machine_host {"VIBESHINE_MACHINE_HOST", ""};
    fs::path base;
    fs::path steam;
    fs::path library;
    fs::path install;
    fs::path proton;
    fs::path runtime;

    static std::string read(const fs::path &path) {
      std::ifstream input(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {});
    }

    static void write(const fs::path &path, const std::string &contents) {
      fs::create_directories(path.parent_path());
      std::ofstream(path, std::ios::binary) << contents;
    }

    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<long long>(std::random_device {}());
      base = fs::temp_directory_path() / ("vibeshine-steam-compatibility-" + std::to_string(nonce));
      steam = base / "Steam client";
      library = base / "External library";
      install = library / "steamapps/common/DualGame";
      // The internal tool name deliberately differs from its directory name.
      proton = steam / "compatibilitytools.d/Custom tool directory";
      runtime = library / "steamapps/common/SteamLinuxRuntime_sniper";
      fs::create_directories(steam / "steamapps");
      fs::create_directories(steam / "appcache");
      fs::create_directories(install / "bin");
      write(steam / "steamapps/libraryfolders.vdf",
        "\"libraryfolders\" { \"0\" { \"path\" \"" + library.generic_string() + "\" } }");
      write(library / "steamapps/appmanifest_588650.acf",
        R"VDF("AppState" { "appid" "588650" "name" "Dual Game" "installdir" "DualGame" "StateFlags" "4" })VDF");
      write_test_appinfo(steam / "appcache/appinfo.vdf", app_id, "Dual Game", true);
      write(install / "Game.exe", "Windows payload; never executed\n");
      write(steam / "userdata/123/config/localconfig.vdf", R"VDF(
        "UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" {
          "588650" { "LaunchOptions" "FIXTURE_OPTION=1 %command% -user" }
        } } } } }
      )VDF");
      write_tool(proton, "FixtureProton", "windows");
      write(runtime / "_v2-entry-point", "Runtime placeholder; never executed\n");
      write(library / "steamapps/appmanifest_99.acf",
        R"VDF("AppState" { "appid" "99" "name" "Steam Linux Runtime" "installdir" "SteamLinuxRuntime_sniper" })VDF");
      write_prefix_history(proton);
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(base, ec);
    }

    void write_tool(const fs::path &path, const std::string &name, const std::string &from_os) {
      write(path / "compatibilitytool.vdf",
        "\"compatibilitytools\" { \"compat_tools\" { \"" + name +
        "\" { \"install_path\" \".\" \"display_name\" \"Fixture tool\" \"from_oslist\" \"" +
        from_os + "\" \"to_oslist\" \"linux\" } } }");
      if (from_os == "windows") {
        write(path / "toolmanifest.vdf", R"VDF("manifest" {
          "version" "2" "commandline" "/proton %verb%" "require_tool_appid" "99" "compatmanager_layer_name" "proton"
        })VDF");
        write(path / "proton", "Proton placeholder; never executed\n");
      } else {
        write(path / "toolmanifest.vdf", R"VDF("manifest" { "version" "2" "commandline" "/_v2-entry-point --verb=%verb% --" })VDF");
        write(path / "_v2-entry-point", "Native runtime placeholder; never executed\n");
      }
    }

    void write_prefix_history(const fs::path &tool) {
      // Proton writes these first four lines while preparing a prefix. They
      // describe a previous launch and can outlive a changed Steam selection.
      write(library / "steamapps/compatdata/588650/config_info",
        "11.0-1\n" + (tool / "files/share/fonts").generic_string() + "/\n" +
        (tool / "files/lib").generic_string() + "/\n" + steam.generic_string() + "\n");
    }

    void mapping(const std::string &entries) {
      write(steam / "config/config.vdf",
        "\"InstallConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"CompatToolMapping\" { " +
        entries + " } } } } }");
    }

    game_t scan(bool catalog = false) {
      const auto config = read(steam / "config/config.vdf");
      const auto prefix = read(library / "steamapps/compatdata/588650/config_info");
      const auto options = read(steam / "userdata/123/config/localconfig.vdf");
      const auto games = catalog ? discover_catalog({steam}) : discover({steam});
      EXPECT_EQ(read(steam / "config/config.vdf"), config);
      EXPECT_EQ(read(library / "steamapps/compatdata/588650/config_info"), prefix);
      EXPECT_EQ(read(steam / "userdata/123/config/localconfig.vdf"), options);
      const auto game = std::find_if(games.begin(), games.end(), [](const auto &value) {
        return value.app_id == app_id;
      });
      EXPECT_NE(game, games.end());
      return game == games.end() ? game_t {} : *game;
    }

    void expect_native() {
      write(install / "Game.sh", "Native payload; never executed\n");
      for (const bool catalog : {false, true}) {
        const auto game = scan(catalog);
        EXPECT_EQ(game.launch_os, "linux");
        EXPECT_EQ(game.launch_executable, install / "Game.sh");
        EXPECT_TRUE(game.proton_path.empty());
        EXPECT_EQ(launch_command(game).find("waitforexitandrun"), std::string::npos);
      }
    }
  };
}  // namespace

TEST_F(SteamCompatibilityOverrides, PerAppProtonSelectsWindowsWithNativeAppinfo) {
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" "priority" "250" })VDF");
  ASSERT_FALSE(fs::exists(install / "Game.sh"));
  for (const bool catalog : {false, true}) {
    const auto game = scan(catalog);
    EXPECT_EQ(game.launch_os, "windows");
    EXPECT_EQ(game.launch_executable, install / "Game.exe");
    EXPECT_EQ(game.launch_working_dir, install / "bin");
    EXPECT_EQ(game.launch_arguments, "-from-appinfo");
    EXPECT_EQ(game.launch_options, "FIXTURE_OPTION=1 %command% -user");
    EXPECT_EQ(game.proton_path, proton);
    EXPECT_EQ(game.proton_runtime_path, runtime);
    EXPECT_EQ(game.steam_client_path, steam);
    const auto command = launch_command(game);
    EXPECT_NE(command.find("Custom tool directory/proton"), std::string::npos);
    EXPECT_NE(command.find("Game.exe"), std::string::npos);
    EXPECT_EQ(command.find("Game.sh"), std::string::npos);
  }
}

TEST_F(SteamCompatibilityOverrides, PerAppSelectionWinsOverGlobalDefault) {
  mapping(R"VDF(
    "0" { "name" "OtherProton" "config" "" "priority" "75" }
    "588650" { "name" "FixtureProton" "config" "" "priority" "250" }
  )VDF");
  for (const bool catalog : {false, true}) {
    const auto game = scan(catalog);
    EXPECT_EQ(game.launch_os, "windows");
    EXPECT_EQ(game.proton_path, proton);
  }
}

TEST_F(SteamCompatibilityOverrides, PrefixHistoryAloneDoesNotForceProton) {
  expect_native();
}

TEST_F(SteamCompatibilityOverrides, GlobalDefaultDoesNotForceNativeGamesToProton) {
  mapping(R"VDF("0" { "name" "FixtureProton" "config" "" "priority" "75" })VDF");
  expect_native();
}

TEST_F(SteamCompatibilityOverrides, EmptyPerAppSelectionRetainsNativePreference) {
  mapping(R"VDF(
    "0" { "name" "FixtureProton" "config" "" "priority" "75" }
    "588650" { "name" "" "config" "" "priority" "250" }
  )VDF");
  expect_native();
}

TEST_F(SteamCompatibilityOverrides, LinuxToLinuxToolIsNotProton) {
  write(install / "Game.sh", "Native payload; never executed\n");
  write_tool(steam / "compatibilitytools.d/NativeRuntime", "FixtureNativeRuntime", "linux");
  mapping(R"VDF("588650" { "name" "FixtureNativeRuntime" "config" "" "priority" "250" })VDF");
  // The selected native runtime may require a Steam-owned launch. Either a
  // correctly wrapped native launch or broker fallback is acceptable here.
  // In particular, stale Proton history must not select a Windows payload.
  for (const bool catalog : {false, true}) {
    const auto game = scan(catalog);
    EXPECT_NE(game.launch_os, "windows");
    EXPECT_TRUE(game.proton_path.empty());
    EXPECT_EQ(launch_command(game).find("Game.exe"), std::string::npos);
    EXPECT_EQ(launch_command(game).find("waitforexitandrun"), std::string::npos);
  }
}

TEST_F(SteamCompatibilityOverrides, UnresolvedOverrideDoesNotRunNativePayload) {
  mapping(R"VDF("588650" { "name" "UninstalledTool" "config" "" "priority" "250" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, ProtonOverrideWithoutWindowsEntryDoesNotRunLinuxThroughProton) {
  write_test_appinfo(steam / "appcache/appinfo.vdf", app_id, "Native only", true, false);
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" "priority" "250" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, ChangedSelectionDoesNotReusePreviousProton) {
  const auto old_proton = steam / "compatibilitytools.d/Previous tool";
  write_tool(old_proton, "PreviousProton", "windows");
  write_prefix_history(old_proton);
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" "priority" "250" })VDF");
  for (const bool catalog : {false, true}) {
    const auto command = launch_command(scan(catalog));
    EXPECT_EQ(command.find("Previous tool/proton"), std::string::npos);
    EXPECT_EQ(command.find("Game.sh"), std::string::npos);
    // Resolving the current tool or declining an incomplete direct launch are
    // both safe; selecting the native binary or the old Proton is not.
    EXPECT_TRUE(command == launch_command(app_id) || command.find("Custom tool directory/proton") != std::string::npos);
  }
}

TEST_F(SteamCompatibilityOverrides, OfficialProtonResolvesInternalNameAndAliasFromSteamPlayManifest) {
  const auto official = library / "steamapps/common/Official Proton";
  write_tool(official, "unused-declaration", "windows");
  fs::remove(official / "compatibilitytool.vdf");
  write(library / "steamapps/appmanifest_199.acf",
    R"VDF("AppState" { "appid" "199" "name" "Official Proton" "installdir" "Official Proton" })VDF");
  write_test_appinfo(steam / "appcache/appinfo.vdf", app_id, "Dual Game", true, true, true);
  // Official Steam installs have no custom compatibilitytool.vdf. The name
  // resolves through appinfo's Steam Play manifest to the installed tool AppID.
  for (const auto name : {"proton_fixture", "proton-fixture-alias"}) {
    mapping("\"588650\" { \"name\" \"" + std::string(name) + "\" \"config\" \"\" }");
    for (const bool catalog : {false, true}) {
      const auto game = scan(catalog);
      EXPECT_EQ(game.launch_os, "windows");
      EXPECT_EQ(game.proton_path, official);
      EXPECT_EQ(game.proton_runtime_path, runtime);
      EXPECT_NE(launch_command(game).find("Official Proton/proton"), std::string::npos);
    }
  }
}

TEST_F(SteamCompatibilityOverrides, MissingWindowsPayloadDoesNotSelectNativeInstead) {
  fs::remove(install / "Game.exe");
  write(install / "Game.sh", "Native payload; never executed\n");
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, UnsupportedToolConfigDeclinesDirectLaunch) {
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "unknown-mode" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, OtherWindowsCompatibilityProtocolDeclinesDirectLaunch) {
  write(proton / "toolmanifest.vdf", R"VDF("manifest" {
    "version" "2" "commandline" "/different-launcher %verb%" "require_tool_appid" "99"
  })VDF");
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, AmbiguousCustomToolDeclinesDirectLaunch) {
  write_tool(steam / "compatibilitytools.d/Duplicate", "FixtureProton", "windows");
  mapping(R"VDF("588650" { "name" "FixtureProton" "config" "" })VDF");
  for (const bool catalog : {false, true}) {
    EXPECT_EQ(launch_command(scan(catalog)), launch_command(app_id));
  }
}

TEST_F(SteamCompatibilityOverrides, DirectCustomDeclarationResolvesRelativeInstallPath) {
  fs::remove(proton / "compatibilitytool.vdf");
  write(steam / "compatibilitytools.d/Fixture.vdf", R"VDF(
    "compatibilitytools" { "compat_tools" { "FixtureProton" {
      "install_path" "Custom tool directory" "from_oslist" "windows" "to_oslist" "linux"
    } } }
  )VDF");
  // Steam's structural keys are case-insensitive; preserve the internal name.
  write(steam / "config/config.vdf", R"VDF(
    "installconfigstore" { "software" { "valve" { "steam" { "compattoolmapping" {
      "588650" { "Name" "FixtureProton" "Config" "" }
    } } } } }
  )VDF");
  for (const bool catalog : {false, true}) {
    const auto game = scan(catalog);
    EXPECT_EQ(game.launch_os, "windows");
    EXPECT_EQ(game.proton_path, proton);
    EXPECT_NE(launch_command(game).find("Custom tool directory/proton"), std::string::npos);
  }
}

TEST_F(SteamCompatibilityOverrides, ValidMetadataSurvivesLaterTruncatedCacheEntry) {
  const auto path = steam / "appcache/appinfo.vdf";
  auto contents = read(path);
  std::uint64_t table = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    table |= static_cast<std::uint64_t>(static_cast<unsigned char>(contents[8 + shift / 8])) << shift;
  }
  ASSERT_GE(table, 4U);
  ASSERT_LE(table, contents.size());
  // Turn the last app sentinel into an incomplete entry header. The valid
  // game before it must survive even though launch resolution is deferred.
  contents[table - 4] = 1;
  write(path, contents);
  expect_native();
}

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

  // Pressure Vessel records host paths beneath /run/host in containerized
  // environments (such as CachyOS Proton SLR). These must normalize to the host root.
  {
    std::ofstream output(compatdata / "config_info");
    output << "External Proton\n"
           << "/run/host" << (external_proton / "files/share/fonts").string() << "/\n"
           << "/run/host" << (external_proton / "files/lib").string() << "/\n"
           << "/run/host" << client.string() << "\n";
  }
  const auto pressure_vessel_games = discover({base});
  ASSERT_GE(pressure_vessel_games.size(), 1U);
  EXPECT_EQ(pressure_vessel_games[0].proton_path, external_proton);
  EXPECT_EQ(pressure_vessel_games[0].steam_client_path, client);
  EXPECT_EQ(pressure_vessel_games[0].proton_runtime_path, runtime);
  EXPECT_NE(launch_command(pressure_vessel_games[0]).find("ExternalProton/proton"), std::string::npos);

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

TEST(SteamPressureVessel, NormalizesContainerHostPaths) {
  EXPECT_EQ(normalize_pressure_vessel_host_path("/run/host/usr/share/steam"), "/usr/share/steam");
  EXPECT_EQ(normalize_pressure_vessel_host_path("/run/host/home/test/.local/share/Steam"), "/home/test/.local/share/Steam");
  EXPECT_EQ(normalize_pressure_vessel_host_path("/run/host"), "/");
  EXPECT_EQ(normalize_pressure_vessel_host_path("/run/host/"), "/");
  EXPECT_EQ(normalize_pressure_vessel_host_path("/usr/share/steam"), "/usr/share/steam");
  EXPECT_EQ(normalize_pressure_vessel_host_path(""), "");
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
  EXPECT_TRUE(requires_direct_environment_launch(true, false, false));
  EXPECT_TRUE(requires_direct_environment_launch(false, true, false));
  EXPECT_TRUE(requires_direct_environment_launch(false, false, true));
  EXPECT_TRUE(requires_direct_environment_launch(true, true, true));
  EXPECT_FALSE(requires_direct_environment_launch(false, false, false));
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
TEST(SteamLaunch, MachineSessionLaunchUsesCanonicalSemanticArguments) {
  session_launch_policy_t policy {
    .provider = "mangohud-proton",
    .limit_millihz = 116000,
    .preset = "3",
    .always_show_graph = true,
    .limiter_method = "late",
    .smooth_motion = true,
    .smooth_motion_graphics_queue = true,
    .hdr = true,
    .wayland_hdr_compatibility = true,
    .proton_dualsense_compatibility = true,
    .playstation_controller_attached = true,
  };
  const auto command = session_launch_command(1182900, policy);
  EXPECT_EQ(
    command,
    "/usr/libexec/vibeshine/vibepollo-session-exec steam-direct "
    "1182900 mangohud-proton 116000 3 1 late 1 1 1 1 1 1"
  );
  const auto arguments = session_launch_arguments(command);
  ASSERT_TRUE(arguments);
  EXPECT_EQ(
    *arguments,
    (std::vector<std::string> {
      "steam-direct", "1182900", "mangohud-proton", "116000", "3",
      "1", "late", "1", "1", "1", "1", "1", "1"
    })
  );

  const auto fallback = session_launch_arguments(
    "/usr/libexec/vibeshine/vibepollo-session-exec steam 1182900"
  );
  ASSERT_TRUE(fallback);
  EXPECT_EQ(*fallback, (std::vector<std::string> {"steam", "1182900"}));
  EXPECT_FALSE(session_launch_arguments(
    "/usr/libexec/vibeshine/vibepollo-session-exec steam 01182900"
  ));
  EXPECT_FALSE(session_launch_arguments(
    "/usr/libexec/vibeshine/vibepollo-session-exec steam 0"
  ));

  policy.provider = "disabled";
  EXPECT_TRUE(session_launch_command(1182900, policy).empty());
  EXPECT_FALSE(session_launch_arguments(command + " trailing"));
  EXPECT_FALSE(session_launch_arguments(
    "/usr/libexec/vibeshine/vibepollo-session-exec steam-direct "
    "1182900 proton 116000 custom 1 late 0 0 0"
  ));
}

TEST(SteamLaunch, DualSenseOnlyPolicyRoundTripsWithoutHdrOrLimiter) {
  session_launch_policy_t policy;
  policy.proton_dualsense_compatibility = true;
  const auto command = session_launch_command(3768760, policy);
  ASSERT_FALSE(command.empty());
  ASSERT_TRUE(session_launch_arguments(command));
  EXPECT_TRUE(command.ends_with(" 0 0 0 0 1 0"));
  auto invalid = command;
  invalid[invalid.size() - 2] = '2';
  EXPECT_FALSE(session_launch_arguments(invalid));
  policy.playstation_controller_attached = true;
  const auto attached = session_launch_command(3768760, policy);
  EXPECT_TRUE(attached.ends_with(" 0 0 0 0 1 1"));
  EXPECT_TRUE(session_launch_arguments(attached));
  policy.proton_dualsense_compatibility = false;
  EXPECT_TRUE(session_launch_command(3768760, policy).empty());
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

TEST(SteamDiscovery, ReadsLastPlayedFromLocalUserData) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto base = fs::temp_directory_path() / ("vibeshine-steam-recent-test-" + std::to_string(nonce));
  fs::create_directories(base / "steamapps/common/Game");
  fs::create_directories(base / "userdata/123/config");
  { std::ofstream out(base / "steamapps/appmanifest_42.acf");
    out << R"("AppState" { "appid" "42" "name" "Game" "installdir" "Game" "LastUpdated" "999" })"; }
  { std::ofstream out(base / "userdata/123/config/localconfig.vdf");
    out << R"("UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" { "42" { "LastPlayed" "123456" } } } } } })"; }
  const auto games = platf::steam::discover_catalog({base});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].last_played, 123456);
  EXPECT_EQ(games[0].last_updated, 999);
  fs::remove_all(base);
}

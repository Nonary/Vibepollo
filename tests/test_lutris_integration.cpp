#include "src/lutris_integration.h"
#include "src/lutris_artwork.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sqlite3.h>

#ifdef VIBESHINE_STEAM_ARTWORK_IMAGE_LIBS
extern "C" {
#include <jpeglib.h>
}
#endif

namespace fs = std::filesystem;

namespace {
  void write_test_cover(const fs::path &path) {
#ifdef VIBESHINE_STEAM_ARTWORK_IMAGE_LIBS
    auto *file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    jpeg_compress_struct compressor {};
    jpeg_error_mgr error {};
    compressor.err = jpeg_std_error(&error);
    jpeg_create_compress(&compressor);
    jpeg_stdio_dest(&compressor, file);
    compressor.image_width = 1;
    compressor.image_height = 1;
    compressor.input_components = 3;
    compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&compressor);
    jpeg_start_compress(&compressor, TRUE);
    unsigned char pixel[] {0x20, 0x80, 0xe0};
    JSAMPROW row = pixel;
    jpeg_write_scanlines(&compressor, &row, 1);
    jpeg_finish_compress(&compressor);
    jpeg_destroy_compress(&compressor);
    std::fclose(file);
#else
    constexpr unsigned char one_pixel_png[] {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
      0x0d, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
      0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
      0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    std::ofstream(path, std::ios::binary).write(
      reinterpret_cast<const char *>(one_pixel_png), static_cast<std::streamsize>(sizeof(one_pixel_png)));
#endif
  }
}

TEST(LutrisDiscovery, ReadsInstalledGamesAndClassifiesSteam) {
  const auto base = fs::temp_directory_path() /
                    ("vibeshine-lutris-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto lutris_data = base / "lutris";
  fs::create_directories(lutris_data / "coverart");
  fs::create_directories(base / "icons/hicolor/128x128/apps");
  const auto path = lutris_data / "pga.db";
  std::ofstream(lutris_data / "coverart/battlenet.jpg").put('x');
  std::ofstream(base / "icons/hicolor/128x128/apps/lutris_battlenet.png").put('x');
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
  EXPECT_EQ(games[0].artwork_path, lutris_data / "coverart/battlenet.jpg");
  EXPECT_EQ(games[0].icon_path, base / "icons/hicolor/128x128/apps/lutris_battlenet.png");
  EXPECT_TRUE(games[0].image_path.empty());
  EXPECT_EQ(games[1].stable_id, "steam:42");
  EXPECT_TRUE(games[1].steam_backed());
  EXPECT_NE(platf::lutris::source_fingerprint(games), 0U);
  fs::remove_all(base);
}

TEST(LutrisArtwork, PreparesPortraitAsProviderSpecificPng) {
  const auto base = fs::temp_directory_path() /
                    ("vibeshine-lutris-artwork-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(base);
  const auto source = base / "portrait.jpg";
  write_test_cover(source);
  platf::lutris::game_t game;
  game.id = 27;
  game.artwork_path = source;
  game.icon_path = base / "icon.png";
  std::vector<platf::lutris::game_t> games {game};
  platf::lutris::artwork::prepare(games, base / "appdata");
  EXPECT_EQ(games[0].image_path, base / "appdata/covers/lutris_27.png");
  EXPECT_TRUE(fs::is_regular_file(games[0].image_path));
  std::error_code error;
  fs::remove_all(base, error);
}

TEST(LutrisLaunch, BuildsValidatedUri) {
  EXPECT_EQ(platf::lutris::launch_uri(7), "lutris:rungameid/7");
  EXPECT_TRUE(platf::lutris::launch_uri(0).empty());
  EXPECT_FALSE(platf::lutris::launch(0));
}

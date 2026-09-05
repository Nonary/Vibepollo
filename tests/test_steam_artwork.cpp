#include "src/steam_artwork.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace fs = std::filesystem;

namespace {
  // A tiny opaque 1x1 PNG used to exercise the decode/encode path without
  // making the test depend on a Steam installation.
  constexpr unsigned char one_pixel_png[] {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
  };

  fs::path test_root() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("vibeshine-steam-artwork-" + std::to_string(nonce));
  }

  void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
  }

  std::uint32_t crc32(const std::vector<std::uint8_t> &bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
      crc ^= byte;
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
    }
    return ~crc;
  }

  void png_chunk(std::vector<std::uint8_t> &png, const char (&type)[5], const std::vector<std::uint8_t> &data) {
    append_u32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> crc_input {type, type + 4};
    crc_input.insert(crc_input.end(), data.begin(), data.end());
    png.insert(png.end(), crc_input.begin(), crc_input.end());
    append_u32(png, crc32(crc_input));
  }

  // A small dependency-free valid 600x900 RGB PNG. Stored DEFLATE blocks keep
  // this fixture deterministic while exercising the real conversion path.
  std::vector<std::uint8_t> full_portrait_png() {
    constexpr std::uint32_t width = 600;
    constexpr std::uint32_t height = 900;
    std::vector<std::uint8_t> png {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> header;
    append_u32(header, width);
    append_u32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", header);

    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 3));
    for (std::uint32_t y = 0; y < height; ++y) {
      raw.push_back(0);
      for (std::uint32_t x = 0; x < width; ++x) {
        raw.push_back(static_cast<std::uint8_t>(x));
        raw.push_back(static_cast<std::uint8_t>(y));
        raw.push_back(0x7f);
      }
    }
    std::vector<std::uint8_t> compressed {0x78, 0x01};
    for (std::size_t offset = 0; offset < raw.size();) {
      const auto count = std::min<std::size_t>(65535, raw.size() - offset);
      const bool last = offset + count == raw.size();
      compressed.push_back(last ? 1 : 0);
      compressed.push_back(static_cast<std::uint8_t>(count));
      compressed.push_back(static_cast<std::uint8_t>(count >> 8));
      const auto inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count));
      compressed.push_back(static_cast<std::uint8_t>(inverse));
      compressed.push_back(static_cast<std::uint8_t>(inverse >> 8));
      compressed.insert(compressed.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                        raw.begin() + static_cast<std::ptrdiff_t>(offset + count));
      offset += count;
    }
    std::uint32_t adler = 1;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto byte : raw) {
      a = (a + byte) % 65521U;
      b = (b + a) % 65521U;
    }
    adler = (b << 16) | a;
    append_u32(compressed, adler);
    png_chunk(png, "IDAT", compressed);
    png_chunk(png, "IEND", {});
    return png;
  }

  std::pair<std::uint32_t, std::uint32_t> png_size(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char> {input}, {});
    if (bytes.size() < 24) return {};
    const auto read = [&](std::size_t offset) {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    return {read(16), read(20)};
  }
}

TEST(SteamArtwork, ConvertsToStablePngAndReusesMatchingFingerprint) {
  const auto root = test_root();
  const auto source = root / "library_600x900.png";
  std::error_code ec;
  fs::create_directories(source.parent_path(), ec);
  {
    std::ofstream output(source, std::ios::binary);
    output.write(reinterpret_cast<const char *>(one_pixel_png), static_cast<std::streamsize>(sizeof(one_pixel_png)));
  }
  const auto first = platf::steam::artwork::sync(2679460, source, root / "appdata");
  ASSERT_FALSE(first.client_path.empty());
  EXPECT_TRUE(first.converted);
  EXPECT_FALSE(first.reused);
  EXPECT_EQ(first.client_path.filename(), "steam_2679460.png");
  EXPECT_TRUE(fs::exists(first.client_path));
  const auto second = platf::steam::artwork::sync(2679460, source, root / "appdata");
  EXPECT_TRUE(second.reused);
  EXPECT_FALSE(second.converted);
  EXPECT_EQ(second.client_path, first.client_path);
  fs::remove_all(root, ec);
}

TEST(SteamArtwork, MissingSourceDoesNotClaimAClientCover) {
  const auto root = test_root();
  const auto result = platf::steam::artwork::sync(42, root / "missing.webp", root / "appdata");
  EXPECT_TRUE(result.client_path.empty());
  EXPECT_FALSE(result.converted);
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST(SteamArtwork, FetchesFullPortraitOnceAndReusesStableCache) {
  const auto root = test_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  const auto source = root / "library_600x900.png";
  std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char *>(one_pixel_png), sizeof(one_pixel_png));
  platf::steam::game_t game;
  game.app_id = 9001;
  game.artwork_path = source;
  auto fixture = full_portrait_png();
  int fetches = 0;
  const auto fetcher = [&](const std::string &url) -> std::optional<std::vector<std::uint8_t>> {
    ++fetches;
    EXPECT_EQ(url, "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/9001/library_600x900_2x.jpg");
    return fixture;
  };
  std::vector<platf::steam::game_t> games {game};
  platf::steam::artwork::prepare(games, root / "appdata", fetcher);
  ASSERT_FALSE(games[0].artwork_client_path.empty());
  EXPECT_EQ(fetches, 1);
  EXPECT_EQ(png_size(games[0].artwork_client_path), std::make_pair(600U, 900U));
  EXPECT_TRUE(fs::is_regular_file(platf::steam::artwork::remote_cache_path(root / "appdata", 9001)));
  platf::steam::artwork::prepare(games, root / "appdata", fetcher);
  EXPECT_EQ(fetches, 1);
  EXPECT_EQ(png_size(games[0].artwork_client_path), std::make_pair(600U, 900U));
  fs::remove_all(root, ec);
}

TEST(SteamArtwork, FailedRemoteFetchKeepsUsableLocalFallback) {
  const auto root = test_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  const auto source = root / "library_600x900.png";
  std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char *>(one_pixel_png), sizeof(one_pixel_png));
  platf::steam::game_t game;
  game.app_id = 9002;
  game.artwork_path = source;
  std::vector<platf::steam::game_t> games {game};
  int fetches = 0;
  platf::steam::artwork::prepare(games, root / "appdata", [&](const std::string &) -> std::optional<std::vector<std::uint8_t>> {
    ++fetches;
    return std::nullopt;
  });
  ASSERT_FALSE(games[0].artwork_client_path.empty());
  EXPECT_EQ(fetches, 1);
  EXPECT_EQ(png_size(games[0].artwork_client_path), std::make_pair(1U, 1U));
  fs::remove_all(root, ec);
}

TEST(SteamArtwork, RejectsInvalidRemoteFixture) {
  const auto root = test_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  const auto source = root / "library_600x900.png";
  std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char *>(one_pixel_png), sizeof(one_pixel_png));
  platf::steam::game_t game;
  game.app_id = 9003;
  game.artwork_path = source;
  std::vector<platf::steam::game_t> games {game};
  auto invalid = full_portrait_png();
  invalid[invalid.size() / 2] ^= 0xff;
  platf::steam::artwork::prepare(games, root / "appdata", [&](const std::string &) {
    return std::optional<std::vector<std::uint8_t>> {invalid};
  });
  ASSERT_FALSE(games[0].artwork_client_path.empty());
  EXPECT_EQ(png_size(games[0].artwork_client_path), std::make_pair(1U, 1U));
  EXPECT_FALSE(fs::is_regular_file(platf::steam::artwork::remote_cache_path(root / "appdata", 9003)));
  fs::remove_all(root, ec);
}

TEST(SteamArtwork, SessionTransferCachesRefreshesAndPreservesCoverOnFailure) {
#if defined(__linux__)
  using namespace platf::steam::artwork;
  const auto root = test_root();
  const auto output = root / "covers/lutris_7.png";
  const auto png = full_portrait_png();
  int calls = 0;
  auto fetch = [&](const std::string &request) -> std::optional<std::vector<std::uint8_t>> {
    EXPECT_EQ(request, "provider-lutris-artwork:7");
    ++calls;
    return png;
  };
  EXPECT_EQ(session_cover("lutris", 7, "123", output, fetch), output);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(session_cover("lutris", 7, "123", output, fetch), output);
  EXPECT_EQ(calls, 1);
  auto broken = [&](const std::string &) -> std::optional<std::vector<std::uint8_t>> {
    ++calls;
    return std::vector<std::uint8_t> {'b', 'a', 'd'};
  };
  EXPECT_EQ(session_cover("lutris", 7, "124", output, broken), output);
  EXPECT_EQ(session_cover("lutris", 7, "124", output, fetch), output);
  EXPECT_EQ(calls, 3);  // Failed transfers never commit the new revision.
  EXPECT_TRUE(session_cover("../../escape", 7, "124", output, fetch).empty());
  EXPECT_TRUE(session_cover("lutris", 0, "124", output, fetch).empty());
  EXPECT_EQ(calls, 3);
  fs::remove_all(root);
#endif
}

TEST(SteamArtwork, ImportedPngRejectsCorruptionWithoutReplacingExistingCover) {
  using namespace platf::steam::artwork;
  const auto root = test_root();
  const auto output = root / "covers/steam_42.png";
  auto png = full_portrait_png();
  ASSERT_TRUE(import_png(png, output));
  png[45] ^= 0xff;
  EXPECT_FALSE(import_png(png, output));
  const auto exported = export_png(output);
  ASSERT_TRUE(exported);
  EXPECT_FALSE(exported->empty());
  EXPECT_FALSE(import_png(std::vector<std::uint8_t>(16U * 1024U * 1024U + 1), output));
  fs::remove_all(root);
}

TEST(SteamArtwork, SessionCoverSurvivesUnavailableCdnWithoutUserPaths) {
#if defined(__linux__)
  using namespace platf::steam::artwork;
  const auto root = test_root();
  const auto local = root / "covers/steam_42_local.png";
  const auto png = full_portrait_png();
  ASSERT_EQ(session_cover("steam", 42, "123", local,
                         [&](const std::string &) { return std::optional {png}; }), local);
  platf::steam::game_t game;
  game.app_id = 42;
  game.session_artwork_revision = "123";
  std::vector<platf::steam::game_t> games {game};
  prepare(games, root, [](const std::string &) -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; });
  EXPECT_EQ(games[0].artwork_client_path, local);
  EXPECT_TRUE(games[0].artwork_path.empty());
  fs::remove_all(root);
#endif
}

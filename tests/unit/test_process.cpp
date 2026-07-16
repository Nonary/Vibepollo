/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test imports
#include "../tests_common.h"

// standard imports
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

// local imports
#include <src/config.h>
#include <src/process.h>

namespace fs = std::filesystem;
namespace pt = boost::property_tree;

#ifdef _WIN32
class DeferredDisplayRevertTest: public ::testing::Test {
protected:
  void SetUp() override {
    proc::clear_deferred_display_revert();
  }

  void TearDown() override {
    proc::clear_deferred_display_revert();
  }
};

TEST_F(DeferredDisplayRevertTest, PersistsUntilFinalSessionConsumesIt) {
  proc::defer_display_revert();

  EXPECT_TRUE(proc::consume_deferred_display_revert());
  EXPECT_FALSE(proc::consume_deferred_display_revert());
}

TEST_F(DeferredDisplayRevertTest, ReplacementAppCancelsPendingRestore) {
  proc::defer_display_revert();

  proc::clear_deferred_display_revert();

  EXPECT_FALSE(proc::consume_deferred_display_revert());
}
#endif

class ProcessPNGTest: public ::testing::Test {
protected:
  void SetUp() override {
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443) - safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

class ProcessAppIdCompatibilityTest: public ::testing::Test {
protected:
  void SetUp() override {
    test_dir = fs::temp_directory_path() / "sunshine_process_app_id_compat_test";  // NOSONAR(cpp:S5443) - safe for tests
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    original_file_state = config::nvhttp.file_state;
    original_vibeshine_file_state = config::nvhttp.vibeshine_file_state;
    config::nvhttp.file_state = (test_dir / "sunshine_state.json").string();
    config::nvhttp.vibeshine_file_state = (test_dir / "vibeshine_state.json").string();
  }

  void TearDown() override {
    config::nvhttp.file_state = original_file_state;
    config::nvhttp.vibeshine_file_state = original_vibeshine_file_state;
    std::error_code ec;
    fs::remove_all(test_dir, ec);
  }

  fs::path writeAppsJson(const std::string &name, const std::string &apps_json) const {
    const auto path = test_dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << apps_json;
    return path;
  }

  fs::path writePng(const std::string &name, unsigned char payload) const {
    const auto path = test_dir / name;
    const std::vector<unsigned char> png_data = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
      payload, static_cast<unsigned char>(payload + 1), static_cast<unsigned char>(payload + 2)
    };
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(png_data.data()), png_data.size());
    return path;
  }

  pt::ptree readState() const {
    pt::ptree tree;
    pt::read_json(config::nvhttp.vibeshine_file_state, tree);
    return tree;
  }

  fs::path test_dir;
  std::string original_file_state;
  std::string original_vibeshine_file_state;
};

TEST_F(ProcessAppIdCompatibilityTest, FirstSeenUuidAppSeedsOldUuidOnlyId) {
  const std::string uuid = "11111111-1111-1111-1111-111111111111";
  const auto cover = writePng("cover-a.png", 0x10);
  const auto apps_path = writeAppsJson(
    "apps-first-seen.json",
    std::string(R"json({"env":{},"apps":[{"name":"Game","uuid":")json") + uuid + R"json(","cmd":"","image-path":")json" + cover.generic_string() + R"json("}]})json"
  );

  auto parsed = proc::parse(apps_path.string());
  ASSERT_TRUE(parsed.has_value());
  const auto apps = parsed->get_apps();
  ASSERT_GE(apps.size(), 1u);

  const auto baseline_ids = proc::calculate_app_id("Game", uuid, cover.generic_string(), 0);
  EXPECT_EQ(apps[0].id, std::get<0>(baseline_ids));
  EXPECT_EQ(apps[0].art_version, proc::calculate_app_cover_fingerprint(cover.generic_string()));
  EXPECT_TRUE(apps[0].id_aliases.empty());
}

TEST_F(ProcessAppIdCompatibilityTest, CoverChangeRotatesCurrentIdAndKeepsOldAlias) {
  const std::string uuid = "22222222-2222-2222-2222-222222222222";
  const auto cover_a = writePng("cover-a.png", 0x20);
  const auto cover_b = writePng("cover-b.png", 0x30);
  const auto apps_path = test_dir / "apps-rotate.json";

  writeAppsJson(
    apps_path.filename().string(),
    std::string(R"json({"env":{},"apps":[{"name":"Game","uuid":")json") + uuid + R"json(","cmd":"","image-path":")json" + cover_a.generic_string() + R"json("}]})json"
  );
  auto first = proc::parse(apps_path.string());
  ASSERT_TRUE(first.has_value());
  const auto old_id = first->get_apps()[0].id;

  writeAppsJson(
    apps_path.filename().string(),
    std::string(R"json({"env":{},"apps":[{"name":"Game","uuid":")json") + uuid + R"json(","cmd":"","image-path":")json" + cover_b.generic_string() + R"json("}]})json"
  );
  auto second = proc::parse(apps_path.string());
  ASSERT_TRUE(second.has_value());
  const auto apps = second->get_apps();
  ASSERT_GE(apps.size(), 1u);

  EXPECT_NE(apps[0].id, old_id);
  EXPECT_NE(std::find(apps[0].id_aliases.begin(), apps[0].id_aliases.end(), old_id), apps[0].id_aliases.end());

  auto resolved = second->resolve_app(old_id);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->uuid, uuid);
  EXPECT_EQ(second->get_app_image(std::stoi(old_id)), cover_b.generic_string());
}

TEST_F(ProcessAppIdCompatibilityTest, AppDeletionPrunesPersistedAliasState) {
  const std::string uuid = "33333333-3333-3333-3333-333333333333";
  const auto apps_path = test_dir / "apps-prune.json";

  writeAppsJson(
    apps_path.filename().string(),
    std::string(R"json({"env":{},"apps":[{"name":"Game","uuid":")json") + uuid + R"json(","cmd":""}]})json"
  );
  ASSERT_TRUE(proc::parse(apps_path.string()).has_value());

  writeAppsJson(apps_path.filename().string(), R"json({"env":{},"apps":[]})json");
  ASSERT_TRUE(proc::parse(apps_path.string()).has_value());

  const auto state = readState();
  const auto aliases = state.get_child_optional("root.app_id_aliases");
  ASSERT_TRUE(aliases.has_value());
  EXPECT_FALSE(aliases->get_child_optional(uuid).has_value());
}

TEST_F(ProcessAppIdCompatibilityTest, AliasConflictsWithCurrentIdIsDropped) {
  const std::string uuid_a = "44444444-4444-4444-4444-444444444444";
  const std::string uuid_b = "55555555-5555-5555-5555-555555555555";

  pt::ptree state;
  pt::ptree aliases_root;
  pt::ptree app_a;
  app_a.put("current_id", "111");
  app_a.put("cover_fingerprint", "default");
  pt::ptree app_a_aliases;
  pt::ptree alias_node;
  alias_node.put_value("222");
  app_a_aliases.push_back(std::make_pair("", alias_node));
  app_a.put_child("aliases", app_a_aliases);
  aliases_root.push_back(std::make_pair(uuid_a, app_a));

  pt::ptree app_b;
  app_b.put("current_id", "222");
  app_b.put("cover_fingerprint", "default");
  app_b.put_child("aliases", pt::ptree {});
  aliases_root.push_back(std::make_pair(uuid_b, app_b));
  state.put_child("root.app_id_aliases", aliases_root);
  pt::write_json(config::nvhttp.vibeshine_file_state, state);

  const auto apps_path = writeAppsJson(
    "apps-collision.json",
    std::string(R"json({"env":{},"apps":[{"name":"A","uuid":")json") + uuid_a + R"json(","cmd":""},{"name":"B","uuid":")json" + uuid_b + R"json(","cmd":""}]})json"
  );
  auto parsed = proc::parse(apps_path.string());
  ASSERT_TRUE(parsed.has_value());

  auto resolved = parsed->resolve_app("222");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->uuid, uuid_b);
  const auto apps = parsed->get_apps();
  const auto app_a_it = std::find_if(apps.begin(), apps.end(), [&](const proc::ctx_t &app) {
    return app.uuid == uuid_a;
  });
  ASSERT_NE(app_a_it, apps.end());
  EXPECT_TRUE(app_a_it->id_aliases.empty());
}

TEST_F(ProcessAppIdCompatibilityTest, DuplicateAliasesAreDroppedInsteadOfResolvedAmbiguously) {
  const std::string uuid_a = "66666666-6666-6666-6666-666666666666";
  const std::string uuid_b = "77777777-7777-7777-7777-777777777777";

  pt::ptree state;
  pt::ptree aliases_root;
  for (const auto &[uuid, current_id] : std::vector<std::pair<std::string, std::string>> {{uuid_a, "301"}, {uuid_b, "302"}}) {
    pt::ptree app;
    app.put("current_id", current_id);
    app.put("cover_fingerprint", "default");
    pt::ptree aliases;
    pt::ptree alias_node;
    alias_node.put_value("999");
    aliases.push_back(std::make_pair("", alias_node));
    app.put_child("aliases", aliases);
    aliases_root.push_back(std::make_pair(uuid, app));
  }
  state.put_child("root.app_id_aliases", aliases_root);
  pt::write_json(config::nvhttp.vibeshine_file_state, state);

  const auto apps_path = writeAppsJson(
    "apps-duplicate-alias.json",
    std::string(R"json({"env":{},"apps":[{"name":"A","uuid":")json") + uuid_a + R"json(","cmd":""},{"name":"B","uuid":")json" + uuid_b + R"json(","cmd":""}]})json"
  );
  auto parsed = proc::parse(apps_path.string());
  ASSERT_TRUE(parsed.has_value());

  EXPECT_FALSE(parsed->resolve_app("999").has_value());
  for (const auto &app : parsed->get_apps()) {
    if (app.uuid == uuid_a || app.uuid == uuid_b) {
      EXPECT_TRUE(app.id_aliases.empty());
    }
  }
}

TEST_F(ProcessAppIdCompatibilityTest, NoUuidIdCalculationStillUsesNameImageAndIndex) {
  const auto cover_a = writePng("legacy-a.png", 0x40);
  const auto cover_b = writePng("legacy-b.png", 0x50);

  const auto first = proc::calculate_app_id("Legacy", "", cover_a.string(), 0);
  const auto second = proc::calculate_app_id("Legacy", "", cover_b.string(), 0);
  const auto indexed = proc::calculate_app_id("Legacy", "", cover_a.string(), 1);

  EXPECT_NE(std::get<0>(first), std::get<0>(second));
  EXPECT_NE(std::get<1>(first), std::get<1>(indexed));
}

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}

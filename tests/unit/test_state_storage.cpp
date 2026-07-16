/**
 * @file tests/unit/test_state_storage.cpp
 * @brief Test src/state_storage.* corrupt-recovery and atomic write behavior.
 */
#include "../tests_common.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <fstream>
#include <src/state_storage.h>

namespace {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  fs::path temp_state_dir() {
    auto dir = fs::temp_directory_path() / "vibeshine_statefile_tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
  }

  void write_raw(const fs::path &p, const std::string &content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
  }

  bool has_quarantine_sibling(const fs::path &original) {
    const std::string prefix = original.filename().string() + ".corrupt-";
    for (const auto &entry : fs::directory_iterator(original.parent_path())) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST(StateStorageLoadForUpdate, MissingFileReturnsTrueWithEmptyTree) {
  const auto path = temp_state_dir() / "missing.json";
  std::error_code ec;
  fs::remove(path, ec);

  pt::ptree tree;
  EXPECT_TRUE(statefile::load_json_for_update(path.string(), tree));
  EXPECT_TRUE(tree.empty());
}

TEST(StateStorageLoadForUpdate, ValidFileLoads) {
  const auto path = temp_state_dir() / "valid.json";
  write_raw(path, R"({"root":{"k":"v"}})");

  pt::ptree tree;
  EXPECT_TRUE(statefile::load_json_for_update(path.string(), tree));
  EXPECT_EQ(tree.get<std::string>("root.k", ""), "v");

  std::error_code ec;
  fs::remove(path, ec);
}

TEST(StateStorageLoadForUpdate, BlankFileTreatedAsMissing) {
  const auto path = temp_state_dir() / "blank.json";
  write_raw(path, "   \r\n\t  ");

  pt::ptree tree;
  EXPECT_TRUE(statefile::load_json_for_update(path.string(), tree));
  EXPECT_TRUE(tree.empty());
  // A blank file is benign, not corrupt: it must NOT be quarantined.
  EXPECT_FALSE(has_quarantine_sibling(path));

  std::error_code ec;
  fs::remove(path, ec);
}

TEST(StateStorageLoadForUpdate, CorruptFileSelfHealsAndIsQuarantined) {
  const auto path = temp_state_dir() / "corrupt.json";
  // Clean up any quarantine files from a previous run.
  for (const auto &entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().rfind("corrupt.json", 0) == 0) {
      std::error_code ec;
      fs::remove(entry.path(), ec);
    }
  }
  write_raw(path, "{ this is : not valid json ]");

  pt::ptree tree;
  // Malformed content must not wedge the writer: it returns true with an empty
  // tree so the caller can recreate the file.
  EXPECT_TRUE(statefile::load_json_for_update(path.string(), tree));
  EXPECT_TRUE(tree.empty());
  // The bad file should have been renamed aside for forensics.
  EXPECT_TRUE(has_quarantine_sibling(path));
  EXPECT_FALSE(fs::exists(path));

  for (const auto &entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().rfind("corrupt.json", 0) == 0) {
      std::error_code ec;
      fs::remove(entry.path(), ec);
    }
  }
}

TEST(StateStorageWriteAtomic, RoundTrips) {
  const auto path = temp_state_dir() / "atomic.json";
  std::error_code ec;
  fs::remove(path, ec);

  pt::ptree tree;
  tree.put("root.hello", "world");
  EXPECT_NO_THROW(statefile::write_json_atomic(path.string(), tree));

  pt::ptree readback;
  pt::read_json(path.string(), readback);
  EXPECT_EQ(readback.get<std::string>("root.hello", ""), "world");

  fs::remove(path, ec);
}

// End-to-end of the reported wedge: a corrupt state file followed by a write must
// succeed and leave valid, re-readable JSON in place.
TEST(StateStorageLoadForUpdate, CorruptThenWriteProducesValidFile) {
  const auto path = temp_state_dir() / "heal_cycle.json";
  for (const auto &entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().rfind("heal_cycle.json", 0) == 0) {
      std::error_code ec;
      fs::remove(entry.path(), ec);
    }
  }
  write_raw(path, "totally not json");

  pt::ptree tree;
  ASSERT_TRUE(statefile::load_json_for_update(path.string(), tree));
  tree.put("root.recovered", "yes");
  EXPECT_NO_THROW(statefile::write_json_atomic(path.string(), tree));

  pt::ptree readback;
  EXPECT_NO_THROW(pt::read_json(path.string(), readback));
  EXPECT_EQ(readback.get<std::string>("root.recovered", ""), "yes");

  for (const auto &entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().rfind("heal_cycle.json", 0) == 0) {
      std::error_code ec;
      fs::remove(entry.path(), ec);
    }
  }
}

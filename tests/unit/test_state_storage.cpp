/**
 * @file tests/unit/test_state_storage.cpp
 * @brief Unit tests for JSON state recovery and atomic-write policy.
 */
#include "../tests_common.h"

#include <boost/property_tree/ptree.hpp>
#include <src/state_storage_policy.h>

#include <map>
#include <string>
#include <vector>

namespace {
  namespace pt = boost::property_tree;
  namespace policy = statefile::policy;

  struct memory_state_store_t {
    std::map<std::string, std::string> files;
    std::vector<std::string> quarantined;

    policy::read_result_t read(const std::string &path) const {
      const auto it = files.find(path);
      return it == files.end() ? policy::read_result_t {policy::read_status_e::missing, {}} :
                                 policy::read_result_t {policy::read_status_e::loaded, it->second};
    }

    void quarantine(const std::string &path) {
      const auto it = files.find(path);
      if (it == files.end()) {
        return;
      }
      const auto quarantine_path = path + ".corrupt-" + std::to_string(quarantined.size());
      files.emplace(quarantine_path, it->second);
      files.erase(it);
      quarantined.push_back(quarantine_path);
    }

    bool write(const std::string &path, const std::string &contents) {
      files[path] = contents;
      return true;
    }
  };

  bool load_for_update(memory_state_store_t &store, const std::string &path, pt::ptree &tree) {
    return policy::load_json_for_update(
             path,
             tree,
             [&store](const std::string &target) { return store.read(target); },
             [&store](const std::string &target) { store.quarantine(target); }) != policy::load_result_e::failed;
  }

  void write_atomic(memory_state_store_t &store, const std::string &path, const pt::ptree &tree) {
    policy::write_json_atomic(
      path,
      tree,
      [&store](const std::string &target, const std::string &contents) { return store.write(target, contents); },
      [&store](const std::string &target) { return store.read(target); });
  }
}  // namespace

TEST(StateStorageLoadForUpdate, MissingFileReturnsTrueWithEmptyTree) {
  memory_state_store_t store;
  pt::ptree tree;

  EXPECT_TRUE(load_for_update(store, "missing.json", tree));
  EXPECT_TRUE(tree.empty());
}

TEST(StateStorageLoadForUpdate, ValidFileLoads) {
  memory_state_store_t store;
  store.files.emplace("valid.json", R"({"root":{"k":"v"}})");
  pt::ptree tree;

  EXPECT_TRUE(load_for_update(store, "valid.json", tree));
  EXPECT_EQ(tree.get<std::string>("root.k", ""), "v");
}

TEST(StateStorageLoadForUpdate, BlankFileTreatedAsMissing) {
  memory_state_store_t store;
  store.files.emplace("blank.json", "   \r\n\t  ");
  pt::ptree tree;

  EXPECT_TRUE(load_for_update(store, "blank.json", tree));
  EXPECT_TRUE(tree.empty());
  EXPECT_TRUE(store.quarantined.empty());
}

TEST(StateStorageLoadForUpdate, CorruptFileSelfHealsAndIsQuarantined) {
  memory_state_store_t store;
  store.files.emplace("corrupt.json", "{ this is : not valid json ]");
  pt::ptree tree;

  EXPECT_TRUE(load_for_update(store, "corrupt.json", tree));
  EXPECT_TRUE(tree.empty());
  ASSERT_EQ(store.quarantined.size(), 1U);
  EXPECT_FALSE(store.files.contains("corrupt.json"));
  EXPECT_TRUE(store.files.contains(store.quarantined.front()));
}

TEST(StateStorageWriteAtomic, RoundTrips) {
  memory_state_store_t store;
  pt::ptree tree;
  tree.put("root.hello", "world");

  EXPECT_NO_THROW(write_atomic(store, "atomic.json", tree));

  pt::ptree readback;
  EXPECT_TRUE(load_for_update(store, "atomic.json", readback));
  EXPECT_EQ(readback.get<std::string>("root.hello", ""), "world");
}

// End-to-end of the reported wedge: a corrupt state file followed by a write must
// succeed and leave valid, re-readable JSON in place.
TEST(StateStorageLoadForUpdate, CorruptThenWriteProducesValidFile) {
  memory_state_store_t store;
  store.files.emplace("heal_cycle.json", "totally not json");
  pt::ptree tree;

  ASSERT_TRUE(load_for_update(store, "heal_cycle.json", tree));
  tree.put("root.recovered", "yes");
  EXPECT_NO_THROW(write_atomic(store, "heal_cycle.json", tree));

  pt::ptree readback;
  EXPECT_TRUE(load_for_update(store, "heal_cycle.json", readback));
  EXPECT_EQ(readback.get<std::string>("root.recovered", ""), "yes");
  ASSERT_EQ(store.quarantined.size(), 1U);
}

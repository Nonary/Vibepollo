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

  policy::load_result_e load_for_read(memory_state_store_t &store, const std::string &path, pt::ptree &tree) {
    return policy::load_json_for_read(
      path,
      tree,
      [&store](const std::string &target) { return store.read(target); }
    );
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

TEST(StateStorageLoadForUpdate, FailedReadDoesNotAuthorizeAnEmptyReplacement) {
  pt::ptree tree;

  const auto result = policy::load_json_for_update(
    "unavailable.json",
    tree,
    [](const std::string &) {
      return policy::read_result_t {policy::read_status_e::failed, {}};
    },
    [](const std::string &) {
      ADD_FAILURE() << "failed reads must not be quarantined";
    }
  );

  EXPECT_EQ(result, policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
}

TEST(StateStorageLoadForRead, CorruptFileIsReportedWithoutQuarantine) {
  memory_state_store_t store;
  store.files.emplace("corrupt.json", "{ not valid json ]");
  pt::ptree tree;

  EXPECT_EQ(load_for_read(store, "corrupt.json", tree), policy::load_result_e::corrupt);
  EXPECT_TRUE(tree.empty());
  EXPECT_TRUE(store.files.contains("corrupt.json"));
  EXPECT_TRUE(store.quarantined.empty());
}

TEST(StateStorageLoadForRead, BlankFileIsNotTreatedAsANewProfile) {
  memory_state_store_t store;
  store.files.emplace("blank.json", "  \r\n\t");
  pt::ptree tree;

  EXPECT_EQ(load_for_read(store, "blank.json", tree), policy::load_result_e::corrupt);
  EXPECT_TRUE(tree.empty());
  EXPECT_TRUE(store.files.contains("blank.json"));
  EXPECT_TRUE(store.quarantined.empty());
}

namespace {
  constexpr auto auxiliary_path = "vibeshine_state.json";
  constexpr auto auxiliary_backup = "vibeshine_state.json.bak";
  constexpr auto auxiliary_snapshot = R"({"root":{"api_tokens":[{"hash":"saved-hash","username":"owner","created_at":"10","scopes":[{"path":"/api/apps","methods":["GET"]}]}],"display_helper_engine":"v2","future_setting":{"keep":"yes"}}})";

  policy::load_result_e load_auxiliary(memory_state_store_t &store, pt::ptree &tree) {
    return policy::load_vibeshine_state(
      auxiliary_path,
      tree,
      [&store](const std::string &path) {
        return store.read(path);
      },
      [&store](const std::string &path, const std::string &contents) {
        return store.write(path, contents);
      }
    );
  }

  void write_auxiliary(memory_state_store_t &store, const pt::ptree &tree) {
    policy::write_vibeshine_state(
      auxiliary_path,
      tree,
      [&store](const std::string &path, const std::string &contents) {
        return store.write(path, contents);
      },
      [&store](const std::string &path) {
        return store.read(path);
      }
    );
  }
}  // namespace

TEST(StateStorageAuxiliary, ExistingInstallSeedsBackupWithoutChangingPrimary) {
  memory_state_store_t store;
  store.files[auxiliary_path] = auxiliary_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(store.files[auxiliary_path], auxiliary_snapshot);
  EXPECT_TRUE(store.files.contains(auxiliary_backup));
  EXPECT_EQ(tree.get<std::string>("root.future_setting.keep"), "yes");
}

TEST(StateStorageAuxiliary, CorruptBlankMissingAndInvalidRootRecoverCompleteBackup) {
  for (const std::string damaged : {"{broken", "", "null", "[]", "{}", R"({"root":"broken"})", R"({"root":{"api_tokens":"broken"}})"}) {
    memory_state_store_t store;
    store.files[auxiliary_path] = damaged;
    store.files[auxiliary_backup] = auxiliary_snapshot;
    pt::ptree tree;
    ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded) << damaged;
    EXPECT_EQ(tree.get<std::string>("root.display_helper_engine"), "v2");
    EXPECT_EQ(tree.get_child("root.api_tokens").front().second.get<std::string>("hash"), "saved-hash");
    EXPECT_EQ(store.files[auxiliary_backup], auxiliary_snapshot);
    pt::ptree restored;
    ASSERT_EQ(load_for_read(store, auxiliary_path, restored), policy::load_result_e::loaded);
    EXPECT_EQ(restored, tree);
  }
  memory_state_store_t store;
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree tree;
  EXPECT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  EXPECT_TRUE(store.files.contains(auxiliary_path));
}

TEST(StateStorageAuxiliary, FreshProfileRequiresBothSnapshotsMissing) {
  memory_state_store_t store;
  pt::ptree tree;
  EXPECT_EQ(load_auxiliary(store, tree), policy::load_result_e::missing);
  store.files[auxiliary_path] = "";
  EXPECT_EQ(load_auxiliary(store, tree), policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(store.files[auxiliary_path], "");
  EXPECT_FALSE(store.files.contains(auxiliary_backup));
  store.files.erase(auxiliary_path);
  store.files[auxiliary_backup] = "broken";
  EXPECT_EQ(load_auxiliary(store, tree), policy::load_result_e::failed);
}

TEST(StateStorageAuxiliary, InvalidBackupNeverRestoresOrOverwritesEvidence) {
  for (const std::string invalid : {"{broken", R"({"root":{"api_tokens":[{"hash":"h","scopes":"all"}]}})", R"({"root":{"session_tokens":[{"hash":"h","expires_at":"not-a-time"}]}})", R"({"root":{"api_tokens":[{"hash":{"bad":"shape"}}]}})", R"({"root":{"api_tokens":[],"api_tokens":[]}})"}) {
    memory_state_store_t store;
    store.files[auxiliary_path] = "damaged";
    store.files[auxiliary_backup] = invalid;
    pt::ptree tree;
    EXPECT_EQ(load_auxiliary(store, tree), policy::load_result_e::failed);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(store.files[auxiliary_path], "damaged");
    EXPECT_EQ(store.files[auxiliary_backup], invalid);
  }
}

TEST(StateStorageAuxiliary, ReadFailureDoesNotRestoreAnOlderAuthorizationSnapshot) {
  pt::ptree tree;
  unsigned writes = 0;
  const auto result = policy::load_vibeshine_state(auxiliary_path, tree, [](const std::string &path) {
    return path == auxiliary_path ? policy::read_result_t {policy::read_status_e::failed, {}} :
                                    policy::read_result_t {policy::read_status_e::loaded, auxiliary_snapshot};
  },
                                                   [&writes](const std::string &, const std::string &) {
                                                     ++writes;
                                                     return true;
                                                   });
  EXPECT_EQ(result, policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(writes, 0U);
}

TEST(StateStorageAuxiliary, FailedRestorePreservesOnlyUsableSnapshot) {
  memory_state_store_t store;
  store.files[auxiliary_path] = "damaged";
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree tree;
  EXPECT_EQ(policy::load_vibeshine_state(auxiliary_path, tree, [&store](const std::string &path) {
              return store.read(path);
            },
                                         [](const std::string &, const std::string &) {
                                           return false;
                                         }),
            policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(store.files[auxiliary_backup], auxiliary_snapshot);
  EXPECT_EQ(store.files[auxiliary_path], "damaged");
}

TEST(StateStorageAuxiliary, MetadataUpdatePreservesRecoveredPermissionsAndUnknownKeys) {
  memory_state_store_t store;
  store.files[auxiliary_path] = "damaged";
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  tree.put("root.last_notified_version", "1.20.0");
  ASSERT_NO_THROW(write_auxiliary(store, tree));
  EXPECT_EQ(store.files[auxiliary_path], store.files[auxiliary_backup]);
  pt::ptree readback;
  ASSERT_EQ(load_auxiliary(store, readback), policy::load_result_e::loaded);
  EXPECT_EQ(readback.get<std::string>("root.future_setting.keep"), "yes");
  EXPECT_EQ(readback.get_child("root.api_tokens"), tree.get_child("root.api_tokens"));
}

TEST(StateStorageAuxiliary, RevocationsReplaceBackupInsteadOfKeepingPreviousTokens) {
  memory_state_store_t store;
  store.files[auxiliary_path] = auxiliary_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  tree.put_child("root.api_tokens", pt::ptree {});
  ASSERT_NO_THROW(write_auxiliary(store, tree));
  store.files[auxiliary_path] = "damaged";
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  EXPECT_TRUE(tree.get_child("root.api_tokens").empty());
}

TEST(StateStorageAuxiliary, ValidPrimaryWinsOverStaleBackup) {
  memory_state_store_t store;
  store.files[auxiliary_path] = R"({"root":{"api_tokens":""}})";
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  EXPECT_TRUE(tree.get_child("root.api_tokens").empty());
  store.files[auxiliary_path] = "damaged";
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  EXPECT_TRUE(tree.get_child("root.api_tokens").empty());
}

TEST(StateStorageAuxiliary, InvalidWriteLeavesBothSnapshotsUntouched) {
  memory_state_store_t store;
  store.files[auxiliary_path] = auxiliary_snapshot;
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree invalid;
  invalid.put("root", "broken");
  EXPECT_THROW(write_auxiliary(store, invalid), std::runtime_error);
  EXPECT_EQ(store.files[auxiliary_path], auxiliary_snapshot);
  EXPECT_EQ(store.files[auxiliary_backup], auxiliary_snapshot);
}

TEST(StateStorageAuxiliary, FailedPrimaryWriteDoesNotRefreshBackup) {
  memory_state_store_t store;
  store.files[auxiliary_path] = auxiliary_snapshot;
  store.files[auxiliary_backup] = auxiliary_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_auxiliary(store, tree), policy::load_result_e::loaded);
  tree.put("root.last_notified_version", "new");
  unsigned writes = 0;
  EXPECT_THROW(policy::write_vibeshine_state(auxiliary_path, tree, [&writes](const std::string &, const std::string &) {
                 ++writes;
                 return false;
               },
                                             [&store](const std::string &path) {
                                               return store.read(path);
                                             }),
               std::runtime_error);
  EXPECT_EQ(writes, 1U);
  EXPECT_EQ(store.files[auxiliary_backup], auxiliary_snapshot);
}

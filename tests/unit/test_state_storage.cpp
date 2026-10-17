/**
 * @file tests/unit/test_state_storage.cpp
 * @brief Unit tests for JSON state recovery and atomic-write policy.
 */
#include "../tests_common.h"

#include <boost/property_tree/ptree.hpp>
#include <src/state_storage_policy.h>
#include <src/paired_state_policy.h>

#include <cstdint>
#include <iterator>
#include <map>
#include <limits>
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

namespace {
  constexpr auto credential_snapshot = R"({"username":"owner","password":"saved-hash","salt":"saved-salt","root":{"uniqueid":"11111111-1111-1111-1111-111111111111","named_devices":[{"perm":3,"allow_client_commands":false}]}})";
  policy::load_result_e recover_credentials(memory_state_store_t &store, const std::string &path = "credentials.json", bool refresh = true) {
    return policy::recover_credentials(path,
      [&store](const std::string &target) { return store.read(target); },
      [&store](const std::string &target, const std::string &contents) { return store.write(target, contents); }, refresh);
  }
}

TEST(StateStorageCredentials, RepairsBeforeCredentialInspectionAndPreservesExactSnapshot) {
  for (const auto damaged : {"broken", "", "[]", R"({"username":{"invalid":"node"},"password":"hash","salt":"salt"})", R"({"username":"owner","password":"hash"})"}) {
    memory_state_store_t store;
    store.files["credentials.json"] = damaged;
    store.files["credentials.json.bak"] = credential_snapshot;
    EXPECT_EQ(recover_credentials(store), policy::load_result_e::loaded);
    EXPECT_EQ(store.files["credentials.json"], credential_snapshot);
    EXPECT_EQ(store.files["credentials.json.bak"], credential_snapshot);
  }
}

TEST(StateStorageCredentials, MissingPrimaryUsesItsOwnBackup) {
  memory_state_store_t store;
  store.files["custom/login.json.bak"] = credential_snapshot;
  store.files["sunshine_state.json.bak"] = "unrelated host credentials";
  EXPECT_EQ(recover_credentials(store, "custom/login.json"), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["custom/login.json"], credential_snapshot);
  EXPECT_EQ(store.files["sunshine_state.json.bak"], "unrelated host credentials");
}

TEST(StateStorageCredentials, FreshCustomPathDoesNotUseHostCredentials) {
  memory_state_store_t store;
  store.files["sunshine_state.json.bak"] = credential_snapshot;
  EXPECT_EQ(recover_credentials(store, "custom/login.json"), policy::load_result_e::missing);
  EXPECT_FALSE(store.files.contains("custom/login.json"));
}

TEST(StateStorageCredentials, ValidPrimaryPasswordChangeWinsOverStaleBackup) {
  memory_state_store_t store;
  store.files["credentials.json"] = R"({"username":"owner","password":"new-hash","salt":"new-salt"})";
  store.files["credentials.json.bak"] = credential_snapshot;
  const auto current = store.files["credentials.json"];
  EXPECT_EQ(recover_credentials(store), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["credentials.json"], current);
  EXPECT_EQ(store.files["credentials.json.bak"], current);
}

TEST(StateStorageCredentials, ManagedBackupRefreshWaitsForFullStateValidation) {
  memory_state_store_t store;
  store.files["sunshine_state.json"] = R"({"username":"owner","password":"hash","salt":"salt","root":{"uniqueid":"invalid-host-id"}})";
  store.files["sunshine_state.json.bak"] = credential_snapshot;
  EXPECT_EQ(recover_credentials(store, "sunshine_state.json", false), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["sunshine_state.json.bak"], credential_snapshot);
}

TEST(StateStorageCredentials, ExplicitCredentialRemovalNeverRestoresOldLogin) {
  memory_state_store_t store;
  store.files["credentials.json"] = "{}";
  store.files["credentials.json.bak"] = credential_snapshot;
  EXPECT_EQ(recover_credentials(store), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["credentials.json"], "{}");
  EXPECT_EQ(store.files["credentials.json.bak"], "{}");
  store.files["credentials.json"] = "broken";
  EXPECT_EQ(recover_credentials(store), policy::load_result_e::failed);
  EXPECT_EQ(store.files["credentials.json"], "broken");
}

TEST(StateStorageCredentials, InvalidRecoveryNeverEnablesCredentialSetup) {
  for (const auto invalid : {"broken", "{}", R"({"username":"owner","salt":"salt"})", R"({"username":"owner","password":"hash","salt":""})", R"({"username":"owner","password":"hash","salt":"salt","root":"broken"})"}) {
    memory_state_store_t store;
    store.files["credentials.json"] = "damaged";
    store.files["credentials.json.bak"] = invalid;
    EXPECT_EQ(recover_credentials(store), policy::load_result_e::failed);
    EXPECT_EQ(store.files["credentials.json"], "damaged");
    EXPECT_EQ(store.files["credentials.json.bak"], invalid);
  }
}

TEST(StateStorageCredentials, FailedReadNeverRestoresAnOlderCredentialSnapshot) {
  unsigned writes = 0;
  EXPECT_EQ(policy::recover_credentials("credentials.json",
    [](const std::string &path) {
      return path.ends_with(".bak") ? policy::read_result_t {policy::read_status_e::loaded, credential_snapshot} : policy::read_result_t {policy::read_status_e::failed, {}};
    }, [&writes](const std::string &, const std::string &) { ++writes; return true; }), policy::load_result_e::failed);
  EXPECT_EQ(writes, 0U);
}

TEST(StateStorageCredentials, FailedRestoreDoesNotDestroyBackup) {
  memory_state_store_t store;
  store.files["credentials.json"] = "damaged";
  store.files["credentials.json.bak"] = credential_snapshot;
  EXPECT_EQ(policy::recover_credentials("credentials.json",
    [&store](const std::string &path) { return store.read(path); },
    [](const std::string &, const std::string &) { return false; }), policy::load_result_e::failed);
  EXPECT_EQ(store.files["credentials.json.bak"], credential_snapshot);
}

namespace {
  constexpr auto paired_snapshot = R"({"username":"owner","password":"hash","salt":"salt","root":{"uniqueid":"11111111-1111-1111-1111-111111111111","named_devices":[{"uuid":"22222222-2222-2222-2222-222222222222","name":"client","cert":"certificate","enabled":"false"}],"api_tokens":[{"hash":"token"}]}})";
  policy::load_result_e load_primary(memory_state_store_t &store, pt::ptree &tree) {
    return policy::load_primary_state_for_update("shared.json", tree,
      [&store](const std::string &path) { return store.read(path); },
      [&store](const std::string &path, const std::string &contents) { return store.write(path, contents); }, policy::valid_primary_state);
  }

  // Synthetic certificate encodings of the same identity exercise selection
  // independently of OpenSSL. Production callers canonicalize parsed X.509.
  std::optional<std::string> certificate_identity(const std::string &certificate) {
    if (certificate == "certificate" || certificate == "alternate-encoding") {
      return "same-client";
    }
    if (certificate == "legacy-only") {
      return "other-client";
    }
    return std::nullopt;
  }

  bool valid_migrated_primary(const pt::ptree &tree, bool allow_bootstrap) {
    auto normalized = tree;
    return policy::normalize_legacy_pairing_state(normalized, certificate_identity) &&
           policy::valid_primary_state(normalized, allow_bootstrap);
  }

  pt::ptree with_legacy_alias(memory_state_store_t &store) {
    store.files["shared.json"] = paired_snapshot;
    pt::ptree tree;
    EXPECT_EQ(load_for_read(store, "shared.json", tree), policy::load_result_e::loaded);
    pt::ptree legacy, device, certs, cert;
    cert.put_value("alternate-encoding");
    certs.push_back({"", cert});
    device.put("uniqueid", "old-client-id");
    device.put_child("certs", certs);
    legacy.push_back({"", device});
    tree.put_child("root.devices", legacy);
    tree.get_child("root.named_devices").front().second.put("hdr_profile", "preserved-profile");
    return tree;
  }
}  // namespace

TEST(StateStorageLegacyPairing, NamedRecordKeepsIdentityRevocationAndSettings) {
  memory_state_store_t store;
  auto tree = with_legacy_alias(store);
  const auto named = tree.get_child("root.named_devices");
  const auto identity = tree.get<std::string>("root.uniqueid");
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(tree, certificate_identity));
  EXPECT_EQ(tree.get_child("root.named_devices"), named);
  EXPECT_EQ(tree.get<std::string>("root.uniqueid"), identity);
  EXPECT_TRUE(tree.get_child("root.devices").front().second.get_child("certs").empty());
  EXPECT_FALSE(tree.get_child("root.named_devices").front().second.get<bool>("enabled"));
  EXPECT_EQ(tree.get_child("root.api_tokens").front().second.get<std::string>("hash"), "token");
}

TEST(StateStorageLegacyPairing, UpgradeSelectsCurrentSnapshotWithoutChangingDurableFiles) {
  memory_state_store_t store;
  const auto tree = with_legacy_alias(store);
  write_atomic(store, "shared.json", tree);
  // A stale recovery snapshot must not re-enable the named record.
  auto stale = tree;
  stale.get_child("root.named_devices").front().second.put("enabled", true);
  write_atomic(store, "shared.json.bak", stale);
  const auto original = store.files;
  pt::ptree selected;
  ASSERT_EQ(policy::load_primary_state_for_update("shared.json", selected, [&store](const std::string &path) {
              return store.read(path);
            },
                                                  [&store](const std::string &path, const std::string &contents) {
                                                    return store.write(path, contents);
                                                  },
                                                  valid_migrated_primary),
            policy::load_result_e::loaded);
  EXPECT_EQ(selected, tree);
  EXPECT_EQ(store.files, original);
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(selected, certificate_identity));
  EXPECT_FALSE(selected.get_child("root.named_devices").front().second.get<bool>("enabled"));
}

TEST(StateStorageLegacyPairing, LegacyOnlyClientsRemainAvailableForImport) {
  memory_state_store_t store;
  auto tree = with_legacy_alias(store);
  pt::ptree cert;
  cert.put_value("legacy-only");
  tree.get_child("root.devices").front().second.get_child("certs").push_back({"", cert});
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(tree, certificate_identity));
  const auto &certs = tree.get_child("root.devices").front().second.get_child("certs");
  ASSERT_EQ(certs.size(), 1U);
  EXPECT_EQ(certs.front().second.get_value<std::string>(), "legacy-only");
  EXPECT_TRUE(policy::valid_primary_state(tree, false));
}

TEST(StateStorageLegacyPairing, RecoveryRestoresLegacySnapshotWithNamedRevocationIntact) {
  memory_state_store_t store;
  const auto tree = with_legacy_alias(store);
  write_atomic(store, "shared.json.bak", tree);
  const auto backup = store.files["shared.json.bak"];
  store.files["shared.json"] = "broken";
  pt::ptree selected;
  ASSERT_EQ(policy::load_primary_state_for_update("shared.json", selected, [&store](const std::string &path) {
              return store.read(path);
            },
                                                  [&store](const std::string &path, const std::string &contents) {
                                                    return store.write(path, contents);
                                                  },
                                                  valid_migrated_primary),
            policy::load_result_e::loaded);
  EXPECT_EQ(store.files["shared.json"], backup);
  EXPECT_EQ(store.files["shared.json.bak"], backup);
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(selected, certificate_identity));
  EXPECT_FALSE(selected.get_child("root.named_devices").front().second.get<bool>("enabled"));
}

TEST(StateStorageLegacyPairing, AmbiguousNamedRecordsAndInvalidCertificatesFailWithoutMutation) {
  for (int scenario = 0; scenario < 5; ++scenario) {
    memory_state_store_t store;
    auto tree = with_legacy_alias(store);
    if (scenario == 0) {
      auto duplicate = tree.get_child("root.named_devices").front().second;
      duplicate.put("uuid", "33333333-3333-3333-3333-333333333333");
      duplicate.put("enabled", true);
      tree.get_child("root.named_devices").push_back({"", duplicate});
    } else if (scenario == 1) {
      tree.get_child("root.devices").front().second.get_child("certs").front().second.put_value("invalid");
    } else if (scenario == 2) {
      tree.get_child("root.named_devices").front().second.put("cert", "invalid");
    } else if (scenario == 3) {
      auto &certs = tree.get_child("root.devices").front().second.get_child("certs");
      auto cert = certs.front().second;
      certs.clear();
      certs.push_back({"malformed-object-key", cert});
    } else {
      pt::ptree cert;
      cert.put_value("invalid");
      // The valid alias is removed in the working copy before this failure.
      tree.get_child("root.devices").front().second.get_child("certs").push_back({"", cert});
    }
    const auto original = tree;
    EXPECT_FALSE(policy::normalize_legacy_pairing_state(tree, certificate_identity));
    EXPECT_EQ(tree, original);
  }
}

namespace {
  // Independent synthetic data shaped like #298: five distinct UUIDs, four
  // certificate identities, string booleans/timestamps, one absent last_seen.
  // These are policy tokens, not PEMs or copies of the reporter's attachments.
  constexpr auto issue298_snapshot = R"({
    "username":"owner","password":"current-hash","salt":"current-salt",
    "root":{"uniqueid":"11111111-1111-1111-1111-111111111111","named_devices":[
      {"uuid":"22222222-2222-2222-2222-222222222221","name":"one","cert":"synthetic-cert-1","enabled":"true"},
      {"uuid":"22222222-2222-2222-2222-222222222222","name":"two","cert":"synthetic-cert-2","enabled":"true","last_seen":"1790870400","perm":"1","config_overrides":{"output_name":"synthetic-output-2"}},
      {"uuid":"22222222-2222-2222-2222-222222222223","name":"three","cert":"synthetic-cert-3","enabled":"true"},
      {"uuid":"22222222-2222-2222-2222-222222222224","name":"four","cert":"synthetic-cert-2","enabled":"true","perm":"2","config_overrides":{"output_name":"synthetic-output-4"}},
      {"uuid":"22222222-2222-2222-2222-222222222225","name":"five","cert":"synthetic-cert-4","enabled":"true"}
    ]}}
  )";

  std::optional<std::string> issue298_certificate_identity(const std::string &certificate) {
    for (const auto known : {"synthetic-cert-1", "synthetic-cert-2", "synthetic-cert-3", "synthetic-cert-4", "synthetic-cert-5"}) {
      if (certificate == known) return certificate;
    }
    return std::nullopt;
  }

  pt::ptree issue298_tree() {
    memory_state_store_t store;
    store.files["synthetic.json"] = issue298_snapshot;
    pt::ptree tree;
    EXPECT_EQ(load_for_read(store, "synthetic.json", tree), policy::load_result_e::loaded);
    return tree;
  }

  bool issue298_valid_snapshot(const pt::ptree &tree, bool allow_bootstrap) {
    auto normalized = tree;
    return policy::normalize_legacy_pairing_state(normalized, issue298_certificate_identity) &&
           policy::valid_primary_state(normalized, allow_bootstrap);
  }

  std::string issue298_diagnostic(const pt::ptree &tree) {
    auto normalized = tree;
    std::string reason;
    (void) policy::normalize_legacy_pairing_state(normalized, issue298_certificate_identity, &reason);
    return reason;
  }
}  // namespace

TEST(StateStorageIssue298, DuplicateNamedIdentitiesAreNotAnAutomaticMergeDecision) {
  for (const bool first_enabled : {false, true}) {
    for (const bool second_enabled : {false, true}) {
      for (const bool same_settings : {false, true}) {
        for (const bool reversed : {false, true}) {
          SCOPED_TRACE(::testing::Message() << first_enabled << second_enabled << same_settings << reversed);
          auto tree = issue298_tree();
          auto &named = tree.get_child("root.named_devices");
          auto &first = std::next(named.begin(), 1)->second;
          auto &second = std::next(named.begin(), 3)->second;
          if (same_settings) {
            const auto uuid = second.get<std::string>("uuid");
            second = first;
            second.put("uuid", uuid);
          }
          first.put("enabled", first_enabled ? "true" : "false");
          second.put("enabled", second_enabled ? "true" : "false");
          if (reversed) first.swap(second);
          const auto original = tree;
          ASSERT_TRUE(policy::valid_primary_state(tree, false));
          EXPECT_FALSE(policy::normalize_legacy_pairing_state(tree, issue298_certificate_identity));
          EXPECT_EQ(tree, original);
        }
      }
    }
  }
}

TEST(StateStorageIssue298, UniqueIdentityControlRetainsLegacyScalarsAndUnknownSettings) {
  auto tree = issue298_tree();
  auto &named = tree.get_child("root.named_devices");
  // This is a control fixture, not an authorized repair of a real pairing.
  named.erase(std::next(named.begin(), 3));
  const auto original = tree;
  ASSERT_EQ(named.size(), 4U);
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(tree, issue298_certificate_identity));
  EXPECT_TRUE(policy::valid_primary_state(tree, false));
  EXPECT_EQ(tree, original);
  EXPECT_EQ(std::next(tree.get_child("root.named_devices").begin(), 1)->second.get<std::int64_t>("last_seen"), 1790870400);
}

TEST(StateStorageIssue298, UnreconciledSnapshotsFailWithoutChangingEitherFile) {
  for (const bool has_backup : {false, true}) {
    memory_state_store_t store;
    store.files["shared.json"] = issue298_snapshot;
    if (has_backup) store.files["shared.json.bak"] = issue298_snapshot;
    const auto original = store.files;
    pt::ptree selected;
    std::string reason;
    unsigned writes = 0;
    EXPECT_EQ(policy::load_primary_state_for_update("shared.json", selected,
                [&store](const std::string &path) { return store.read(path); },
                [&store, &writes](const std::string &path, const std::string &contents) {
                  ++writes;
                  return store.write(path, contents);
                }, issue298_valid_snapshot, {}, &reason), policy::load_result_e::failed);
    EXPECT_TRUE(selected.empty());
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(store.files, original);
    EXPECT_TRUE(store.quarantined.empty());
    EXPECT_FALSE(reason.empty());
  }
}

// The frozen-master negative control restored stale authorization here.
TEST(StateStorageIssue298, AmbiguousCurrentPrimaryMustNotRestoreOlderAuthorization) {
  memory_state_store_t store;
  auto current = issue298_tree();
  auto stale = current;
  auto &stale_named = stale.get_child("root.named_devices");
  stale_named.erase(std::next(stale_named.begin(), 3));
  stale.put("password", "old-hash");
  stale.put("salt", "old-salt");
  ASSERT_TRUE(issue298_valid_snapshot(stale, false));
  // A newer explicit revocation and credential change must not disappear just
  // because another record makes the current snapshot ambiguous.
  current.get_child("root.named_devices").front().second.put("enabled", "false");
  std::next(current.get_child("root.named_devices").begin(), 1)->second.put("enabled", "false");
  pt::ptree newly_paired;
  newly_paired.put("uuid", "22222222-2222-2222-2222-222222222226");
  newly_paired.put("name", "newly paired");
  newly_paired.put("cert", "synthetic-cert-5");
  newly_paired.put("enabled", "true");
  current.get_child("root.named_devices").push_back({"", newly_paired});
  write_atomic(store, "shared.json", current);
  write_atomic(store, "shared.json.bak", stale);
  const auto original = store.files;
  const auto read = [&store](const std::string &path) { return store.read(path); };
  unsigned writes = 0;
  const auto write = [&store, &writes](const std::string &path, const std::string &contents) {
    ++writes;
    return store.write(path, contents);
  };
  // Match managed-state credential startup: it does not refresh the backup
  // until the complete pairing snapshot has been accepted.
  ASSERT_EQ(policy::recover_credentials("shared.json", read, write, false), policy::load_result_e::loaded);
  pt::ptree selected;
  std::string reason;
  EXPECT_EQ(policy::load_primary_state_for_update("shared.json", selected, read, write,
              issue298_valid_snapshot, {}, &reason), policy::load_result_e::failed);
  EXPECT_TRUE(selected.empty());
  EXPECT_EQ(writes, 0U);
  EXPECT_EQ(store.files, original);
  EXPECT_TRUE(store.quarantined.empty());
  EXPECT_FALSE(reason.empty());
}

TEST(StateStorageIssue298, DiagnosticsNameConflictIndicesWithoutExposingRecordContents) {
  constexpr auto conflict = "root.named_devices[3].cert duplicates root.named_devices[1].cert; distinct pairing records require administrator reconciliation";
  auto tree = issue298_tree();
  const auto original = tree;
  std::string reason = "old diagnostic";
  ASSERT_FALSE(policy::normalize_legacy_pairing_state(tree, issue298_certificate_identity, &reason));
  EXPECT_EQ(reason, conflict);
  EXPECT_EQ(tree, original);
  for (const bool readable_primary : {false, true}) {
    memory_state_store_t store;
    store.files["shared.json"] = readable_primary ? issue298_snapshot : "broken";
    store.files["shared.json.bak"] = issue298_snapshot;
    const auto files = store.files;
    pt::ptree selected;
    unsigned writes = 0;
    EXPECT_EQ(policy::load_primary_state_for_update("shared.json", selected,
                [&store](const std::string &path) { return store.read(path); },
                [&writes](const std::string &, const std::string &) { ++writes; return true; },
                issue298_valid_snapshot, {}, &reason, issue298_diagnostic), policy::load_result_e::failed);
    const auto expected = readable_primary ?
      std::string("primary: ") + conflict + "; refusing recovery snapshot to avoid authorization rollback" :
      std::string("primary: blank or malformed JSON; recovery: ") + conflict;
    EXPECT_EQ(reason, expected);
    EXPECT_TRUE(selected.empty());
    EXPECT_EQ(writes, 0U);
    EXPECT_EQ(store.files, files);
  }
  tree.get_child("root.named_devices").erase(std::next(tree.get_child("root.named_devices").begin(), 3));
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(tree, issue298_certificate_identity, &reason));
  EXPECT_TRUE(reason.empty());
}

TEST(StateStoragePrimary, ReadableRejectedAuthorizationCannotRestoreOlderCredentials) {
  memory_state_store_t store;
  store.files["shared.json.bak"] = paired_snapshot;
  pt::ptree current;
  ASSERT_EQ(load_for_read(store, "shared.json.bak", current), policy::load_result_e::loaded);
  current.put("password", "new-hash");
  current.get_child("root.named_devices").front().second.put("enabled", "invalid");
  write_atomic(store, "shared.json", current);
  const auto original = store.files;
  ASSERT_EQ(recover_credentials(store, "shared.json", false), policy::load_result_e::loaded);
  EXPECT_EQ(store.files, original);
  pt::ptree selected;
  EXPECT_EQ(load_primary(store, selected), policy::load_result_e::failed);
  EXPECT_TRUE(selected.empty());
  EXPECT_EQ(store.files, original);
}

TEST(StateStoragePrimary, FailureDiagnosticsDistinguishCorruptionAndRecoveryWriteFailure) {
  memory_state_store_t store;
  store.files["shared.json"] = "broken";
  pt::ptree tree;
  std::string reason;
  const auto read = [&store](const std::string &path) {
    return store.read(path);
  };
  const auto refuse_write = [](const std::string &, const std::string &) {
    return false;
  };
  EXPECT_EQ(policy::load_primary_state_for_update("shared.json", tree, read, refuse_write, policy::valid_primary_state, {}, &reason), policy::load_result_e::failed);
  EXPECT_EQ(reason, "primary: blank or malformed JSON; recovery: missing");
  store.files["shared.json.bak"] = paired_snapshot;
  EXPECT_EQ(policy::load_primary_state_for_update("shared.json", tree, read, refuse_write, policy::valid_primary_state, {}, &reason), policy::load_result_e::failed);
  EXPECT_EQ(reason, "could not restore the validated recovery snapshot");
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(store.files["shared.json"], "broken");
  EXPECT_EQ(store.files["shared.json.bak"], paired_snapshot);
}

TEST(StateStoragePrimary, SharedMetadataUpdateRecoversIdentityAndAuthorization) {
  memory_state_store_t store;
  store.files["shared.json"] = "broken";
  store.files["shared.json.bak"] = paired_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_primary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["shared.json"], paired_snapshot);
  EXPECT_FALSE(tree.get_child("root.named_devices").front().second.get<bool>("enabled"));
  tree.put("root.display_helper_engine", "v2");
  EXPECT_TRUE(policy::primary_write_allowed(tree, policy::load_result_e::loaded, tree, policy::valid_primary_state));
  EXPECT_EQ(store.files["shared.json.bak"], paired_snapshot);
}

TEST(StateStoragePrimary, MissingPrimaryRecoversBeforeMetadataCanCreatePartialState) {
  memory_state_store_t store;
  store.files["shared.json.bak"] = paired_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_primary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(tree.get<std::string>("root.uniqueid"), "11111111-1111-1111-1111-111111111111");
}

TEST(StateStoragePrimary, PartialMetadataCannotReplaceAnExistingIdentityBackup) {
  memory_state_store_t store;
  store.files["shared.json.bak"] = paired_snapshot;
  pt::ptree backup;
  ASSERT_EQ(load_for_read(store, "shared.json.bak", backup), policy::load_result_e::loaded);
  pt::ptree partial;
  partial.put("root.display_helper_engine", "v2");
  EXPECT_FALSE(policy::primary_write_allowed(partial, policy::load_result_e::loaded, backup, policy::valid_primary_state));
  store.files["shared.json"] = R"({"root":{"display_helper_engine":"v2"}})";
  EXPECT_EQ(load_primary(store, partial), policy::load_result_e::failed);
  EXPECT_EQ(store.files["shared.json.bak"], paired_snapshot);
}

TEST(StateStoragePrimary, CorruptSnapshotsFailClosedWithoutQuarantine) {
  memory_state_store_t store;
  store.files["shared.json"] = "broken";
  store.files["shared.json.bak"] = R"({"root":{"uniqueid":{"invalid":"shape"}}})";
  pt::ptree tree;
  EXPECT_EQ(load_primary(store, tree), policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_TRUE(store.quarantined.empty());
  EXPECT_EQ(store.files["shared.json"], "broken");
}

TEST(StateStoragePrimary, BootstrapCredentialsAndMetadataAreAllowedOnlyWithoutPriorIdentity) {
  memory_state_store_t store;
  pt::ptree tree;
  EXPECT_EQ(load_primary(store, tree), policy::load_result_e::missing);
  tree.put("username", "owner");
  tree.put("password", "hash");
  tree.put("salt", "salt");
  EXPECT_TRUE(policy::primary_write_allowed(tree, policy::load_result_e::missing, {}, policy::valid_primary_state));
  EXPECT_TRUE(policy::valid_primary_state(tree, true));
  EXPECT_FALSE(policy::valid_primary_state(tree, false));
  tree.put("root.display_helper_engine", "v2");
  EXPECT_TRUE(policy::primary_write_allowed(tree, policy::load_result_e::missing, {}, policy::valid_primary_state));
  EXPECT_FALSE(policy::primary_write_allowed(tree, policy::load_result_e::failed, {}, policy::valid_primary_state));
}

TEST(StateStoragePrimary, SemanticClientDamageCannotRefreshRecoveryCopy) {
  memory_state_store_t store;
  store.files["shared.json"] = paired_snapshot;
  pt::ptree tree;
  ASSERT_EQ(load_for_read(store, "shared.json", tree), policy::load_result_e::loaded);
  auto damaged = tree;
  damaged.get_child("root.named_devices").front().second.put("enabled", "invalid");
  EXPECT_FALSE(policy::primary_write_allowed(damaged, policy::load_result_e::loaded, tree, policy::valid_primary_state));
  damaged = tree;
  damaged.get_child("root.named_devices").front().second.put("uuid", "invalid-uuid");
  EXPECT_FALSE(policy::primary_write_allowed(damaged, policy::load_result_e::loaded, tree, policy::valid_primary_state));
}

TEST(StateStoragePrimary, StorageReadFailureDoesNotRestoreOldAuthorization) {
  pt::ptree tree;
  unsigned writes = 0;
  EXPECT_EQ(policy::load_primary_state_for_update("shared.json", tree,
    [](const std::string &) { return policy::read_result_t {policy::read_status_e::failed, {}}; },
    [&writes](const std::string &, const std::string &) { ++writes; return true; }, policy::valid_primary_state), policy::load_result_e::failed);
  EXPECT_EQ(writes, 0U);
}

namespace {
  // Compose credential startup, authoritative host selection, then a later
  // metadata save against the same durable files. A refused selection must be
  // final at both call sites; neither may invent its own backup fallback.
  bool save_running_host_metadata(memory_state_store_t &store, const std::string &identity, bool new_tls_profile = false) {
    const auto read = [&store](const std::string &path) { return store.read(path); };
    const auto write = [&store](const std::string &path, const std::string &contents) { return store.write(path, contents); };
    pt::ptree saved;
    const auto save_selected = policy::load_primary_state_for_update("shared.json", saved, read, write, policy::valid_primary_state);
    if (save_selected != policy::load_result_e::loaded && !(new_tls_profile && save_selected == policy::load_result_e::missing)) return false;
    saved.put("root.uniqueid", identity);
    saved.put("root.display_helper_engine", "v2");
    pt::ptree backup;
    const auto backup_status = policy::load_json_for_read("shared.json.bak", backup, read);
    if (!policy::primary_write_allowed(saved, backup_status, backup, policy::valid_primary_state)) return false;
    write_atomic(store, "shared.json", saved);
    write_atomic(store, "shared.json.bak", saved);
    return true;
  }

  bool startup_then_save(memory_state_store_t &store, bool new_tls_profile) {
    const auto read = [&store](const std::string &path) { return store.read(path); };
    const auto write = [&store](const std::string &path, const std::string &contents) { return store.write(path, contents); };
    if (policy::recover_credentials("shared.json", read, write, false) == policy::load_result_e::failed) return false;
    pt::ptree startup;
    const auto selected = policy::load_primary_state_for_update("shared.json", startup, read, write, policy::valid_primary_state);
    if (selected == policy::load_result_e::failed) return false;
    if (!policy::valid_primary_state(startup, false) && !new_tls_profile) return false;
    const auto identity = startup.get<std::string>("root.uniqueid", "33333333-3333-3333-3333-333333333333");
    return save_running_host_metadata(store, identity, new_tls_profile);
  }
}

TEST(StateStorageComposedStartup, CurrentCredentialOnlyPrimaryNeverRollsBackToPairedBackup) {
  memory_state_store_t store;
  store.files["shared.json"] = R"({"username":"owner","password":"new-hash","salt":"new-salt"})";
  store.files["shared.json.bak"] = paired_snapshot;
  const auto original = store.files;
  EXPECT_FALSE(startup_then_save(store, false));
  EXPECT_EQ(store.files, original);
  // Even an incorrectly broad first-run retry cannot override the refusal.
  EXPECT_FALSE(startup_then_save(store, true));
  EXPECT_EQ(store.files, original);
}

TEST(StateStorageComposedStartup, CredentialOrMetadataBootstrapWithoutPriorIdentityStillWorks) {
  for (const auto initial : {R"({"username":"owner","password":"new-hash","salt":"new-salt"})", R"({"root":{"display_helper_engine":"v2"}})"}) {
    memory_state_store_t store;
    store.files["shared.json"] = initial;
    EXPECT_TRUE(startup_then_save(store, true));
    pt::ptree saved;
    ASSERT_EQ(load_for_read(store, "shared.json", saved), policy::load_result_e::loaded);
    EXPECT_EQ(saved.get<std::string>("root.uniqueid"), "33333333-3333-3333-3333-333333333333");
  }
  memory_state_store_t fresh;
  EXPECT_TRUE(startup_then_save(fresh, true));
}

TEST(StateStorageComposedStartup, CorruptPrimaryRestoresIdentityAndCredentialsBeforeSavingMetadata) {
  memory_state_store_t store;
  store.files["shared.json"] = "broken";
  store.files["shared.json.bak"] = paired_snapshot;
  ASSERT_TRUE(startup_then_save(store, false));
  pt::ptree saved;
  ASSERT_EQ(load_for_read(store, "shared.json", saved), policy::load_result_e::loaded);
  EXPECT_EQ(saved.get<std::string>("root.uniqueid"), "11111111-1111-1111-1111-111111111111");
  EXPECT_EQ(saved.get<std::string>("password"), "hash");
  EXPECT_FALSE(saved.get_child("root.named_devices").front().second.get<bool>("enabled"));
  EXPECT_EQ(store.files["shared.json"], store.files["shared.json.bak"]);
}

TEST(StateStorageComposedStartup, ValidCurrentPasswordWinsAndRemainsCurrentAfterSave) {
  memory_state_store_t store;
  store.files["shared.json.bak"] = paired_snapshot;
  pt::ptree changed;
  ASSERT_EQ(load_for_read(store, "shared.json.bak", changed), policy::load_result_e::loaded);
  changed.put("password", "new-hash");
  changed.put("salt", "new-salt");
  write_atomic(store, "shared.json", changed);
  ASSERT_TRUE(startup_then_save(store, false));
  pt::ptree saved;
  ASSERT_EQ(load_for_read(store, "shared.json.bak", saved), policy::load_result_e::loaded);
  EXPECT_EQ(saved.get<std::string>("password"), "new-hash");
  EXPECT_EQ(saved.get<std::string>("salt"), "new-salt");
}

TEST(StateStorageComposedStartup, RunningHostSaveCannotRollBackChangedCredentialOnlyPrimary) {
  memory_state_store_t store;
  store.files["shared.json"] = paired_snapshot;
  ASSERT_TRUE(startup_then_save(store, false));
  // Credentials changed after the host loaded its in-memory client snapshot.
  store.files["shared.json"] = R"({"username":"owner","password":"new-hash","salt":"new-salt"})";
  const auto original = store.files;
  EXPECT_FALSE(save_running_host_metadata(store, "11111111-1111-1111-1111-111111111111"));
  EXPECT_EQ(store.files, original);
}


namespace {
  const auto apollo_snapshot = R"({"root":{"uniqueid":"11111111-1111-1111-1111-111111111111","remote_display_layout":"{}","named_devices":[{"uuid":"22222222-2222-2222-2222-222222222222","cert":"certificate","name":"Client","perm":15,"allow_client_commands":true,"do":[{"cmd":"start","elevated":false}],"undo":[{"cmd":"stop","elevated":true}],"config_overrides":{"fps":"60"},"future_field":{"count":7}}]}})";

  policy::load_result_e load_apollo_primary(memory_state_store_t &store, pt::ptree &tree) {
    return policy::load_primary_state_for_update("shared.json", tree,
      [&store](const std::string &path) { return store.read(path); },
      [&store](const std::string &path, const std::string &contents) { return store.write(path, contents); },
      nvhttp::state_policy::valid_primary_tree, nvhttp::state_policy::valid_primary_json);
  }

  pt::ptree apollo_tree(const nlohmann::json &snapshot) {
    pt::ptree tree;
    std::istringstream input(snapshot.dump());
    pt::read_json(input, tree);
    return tree;
  }
}

TEST(StateStorageApollo, PopulatedLastSeenLoadsWithoutReplacingCurrentPrimary) {
  for (const auto &timestamp : {nlohmann::json(1759000000), nlohmann::json(0), nlohmann::json(-1),
         nlohmann::json(std::numeric_limits<std::int64_t>::min()), nlohmann::json(std::numeric_limits<std::int64_t>::max()),
         nlohmann::json("1759000000"), nlohmann::json("-1"), nlohmann::json("0"),
         nlohmann::json("-9223372036854775808"), nlohmann::json("9223372036854775807"), nlohmann::json(nullptr)}) {
    SCOPED_TRACE(timestamp.dump());
    memory_state_store_t store;
    auto snapshot = nlohmann::json::parse(apollo_snapshot);
    snapshot["root"]["named_devices"][0]["last_seen"] = timestamp;
    store.files["shared.json"] = snapshot.dump();
    store.files["shared.json.bak"] = apollo_snapshot;
    const auto original = store.files;
    pt::ptree tree;
    ASSERT_EQ(load_apollo_primary(store, tree), policy::load_result_e::loaded);
    EXPECT_EQ(store.files, original);
    auto selected = nlohmann::json::parse(store.files["shared.json"]);
    ASSERT_TRUE(nvhttp::state_policy::normalize_snapshot(selected));
    const auto &last_seen = selected["root"]["named_devices"][0]["last_seen"];
    if (timestamp.is_null()) {
      EXPECT_TRUE(last_seen.is_null());
    } else {
      EXPECT_TRUE(last_seen.is_number_integer());
      EXPECT_EQ(last_seen.get<std::int64_t>(), tree.get_child("root.named_devices").front().second.get<std::int64_t>("last_seen"));
    }
  }
}

TEST(StateStorageApollo, PopulatedLastSeenAllowsNewHostPersistenceAndMetadataUpdate) {
  memory_state_store_t store;
  auto snapshot = nlohmann::json::parse(apollo_snapshot);
  snapshot["root"]["named_devices"][0]["last_seen"] = 1759000000;
  auto tree = apollo_tree(snapshot);
  ASSERT_TRUE(nvhttp::state_policy::valid_primary_json(snapshot.dump()));
  ASSERT_TRUE(policy::primary_write_allowed(tree, policy::load_result_e::missing, {}, nvhttp::state_policy::valid_primary_tree));
  store.files["shared.json"] = snapshot.dump();
  store.files["shared.json.bak"] = snapshot.dump();
  ASSERT_EQ(load_apollo_primary(store, tree), policy::load_result_e::loaded);
  const auto backup = tree;
  tree.put("root.last_notified_version", "2.0.0");
  tree.get_child("root.named_devices").front().second.put("last_seen", 1759000010);
  ASSERT_TRUE(policy::primary_write_allowed(tree, policy::load_result_e::loaded, backup, nvhttp::state_policy::valid_primary_tree));

  nlohmann::json persisted;
  ASSERT_TRUE(nvhttp::state_policy::primary_tree_json(tree, persisted));
  ASSERT_TRUE(nvhttp::state_policy::valid_primary_json(persisted.dump()));
  EXPECT_TRUE(persisted["root"]["named_devices"][0]["last_seen"].is_number_integer());
  store.write("shared.json", persisted.dump());
  store.write("shared.json.bak", persisted.dump());
  ASSERT_EQ(load_apollo_primary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(tree.get<std::string>("root.last_notified_version"), "2.0.0");
  EXPECT_EQ(tree.get_child("root.named_devices").front().second.get<std::int64_t>("last_seen"), 1759000010);
  EXPECT_EQ(store.files["shared.json"], store.files["shared.json.bak"]);

  store.files["shared.json"] = "broken";
  ASSERT_EQ(load_apollo_primary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["shared.json"], persisted.dump());
  EXPECT_EQ(store.files["shared.json.bak"], persisted.dump());
  EXPECT_EQ(tree.get_child("root.named_devices").front().second.get<std::int64_t>("last_seen"), 1759000010);
}

TEST(StateStorageApollo, NullLastSeenSurvivesMetadataSerializationAndBackupRecovery) {
  memory_state_store_t store;
  auto snapshot = nlohmann::json::parse(apollo_snapshot);
  snapshot["root"]["named_devices"][0]["last_seen"] = nullptr;
  auto tree = apollo_tree(snapshot);
  ASSERT_TRUE(nvhttp::state_policy::valid_primary_tree(tree, false));
  nlohmann::json persisted;
  ASSERT_TRUE(nvhttp::state_policy::primary_tree_json(tree, persisted));
  EXPECT_TRUE(persisted["root"]["named_devices"][0]["last_seen"].is_null());
  store.files["shared.json"] = "broken";
  store.files["shared.json.bak"] = persisted.dump();
  ASSERT_EQ(load_apollo_primary(store, tree), policy::load_result_e::loaded);
  EXPECT_EQ(store.files["shared.json"], persisted.dump());
}

TEST(StateStorageApollo, TypedClientDamageFailsClosedWithoutRestoringOlderBackup) {
  for (const auto key : {"name", "display_mode", "hdr_profile", "output_name_override", "virtual_display_mode", "virtual_display_layout"}) {
    SCOPED_TRACE(key);
    memory_state_store_t store;
    auto damaged = nlohmann::json::parse(apollo_snapshot);
    damaged["root"]["named_devices"][0][key] = 123;
    store.files["shared.json"] = damaged.dump();
    store.files["shared.json.bak"] = apollo_snapshot;
    const auto original = store.files;
    pt::ptree tree;
    EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(store.files, original);
  }
}

TEST(StateStorageApollo, InvalidTypedBackupIsNeverRestored) {
  memory_state_store_t store;
  auto damaged = nlohmann::json::parse(apollo_snapshot);
  damaged["root"]["named_devices"][0]["config_overrides"]["fps"] = 60;
  store.files["shared.json"] = "broken";
  store.files["shared.json.bak"] = damaged.dump();
  pt::ptree tree;
  EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
  EXPECT_EQ(store.files["shared.json"], "broken");
  EXPECT_EQ(store.files["shared.json.bak"], damaged.dump());
}

TEST(StateStorageApollo, InvalidTypedLayoutFailsClosedWithoutRestoringOlderBackup) {
  memory_state_store_t store;
  auto damaged = nlohmann::json::parse(apollo_snapshot);
  damaged["root"]["remote_display_layout"] = nlohmann::json::object();
  EXPECT_FALSE(nvhttp::state_policy::valid_primary_json(damaged.dump()));
  store.files["shared.json"] = damaged.dump();
  store.files["shared.json.bak"] = apollo_snapshot;
  const auto original = store.files;
  pt::ptree tree;
  EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(store.files, original);
}


TEST(StateStorageApollo, InvalidNotificationVersionFailsClosedWithoutRestoringOlderBackup) {
  memory_state_store_t store;
  auto damaged = nlohmann::json::parse(apollo_snapshot);
  damaged["root"]["last_notified_version"] = 123;
  store.files["shared.json"] = damaged.dump();
  store.files["shared.json.bak"] = apollo_snapshot;
  const auto original = store.files;
  pt::ptree tree;
  EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(store.files, original);
}

TEST(StateStorageApollo, LegacyNumericBooleansRemainUsableByTypedClientAndCommandParser) {
  for (int value : {0, 1}) {
    auto snapshot = nlohmann::json::parse(apollo_snapshot);
    auto &device = snapshot["root"]["named_devices"][0];
    for (const auto key : {"enable_legacy_ordering", "allow_client_commands", "always_use_virtual_display", "prefer_10bit_sdr"}) device[key] = value;
    device["do"][0]["elevated"] = value;
    device["undo"][0]["elevated"] = value;
    ASSERT_TRUE(nvhttp::state_policy::valid_primary_json(snapshot.dump()));
    ASSERT_TRUE(nvhttp::state_policy::normalize_snapshot(snapshot));
    for (const auto key : {"enable_legacy_ordering", "allow_client_commands", "always_use_virtual_display", "prefer_10bit_sdr"}) EXPECT_EQ(device[key].get<bool>(), value != 0);
    EXPECT_EQ(device["do"][0]["elevated"].get<bool>(), value != 0);
    EXPECT_EQ(device["undo"][0]["elevated"].get<bool>(), value != 0);
  }
}


TEST(StateStorageApollo, InvalidLastSeenFailsClosedWithoutRestoringOlderBackup) {
  auto backup = nlohmann::json::parse(apollo_snapshot);
  backup["root"]["named_devices"][0]["last_seen"] = 1759000000;
  for (const auto &invalid : {nlohmann::json(true), nlohmann::json(false), nlohmann::json(1.0), nlohmann::json(1.5),
         nlohmann::json(std::numeric_limits<std::uint64_t>::max()), nlohmann::json(std::uint64_t(std::numeric_limits<std::int64_t>::max()) + 1),
         nlohmann::json("bad"), nlohmann::json(""), nlohmann::json("null"), nlohmann::json("1tail"),
         nlohmann::json(" 1"), nlohmann::json("1 "), nlohmann::json("+1"), nlohmann::json("-"),
         nlohmann::json("01"), nlohmann::json("-01"), nlohmann::json("1.0"), nlohmann::json("1e3"),
         nlohmann::json("9223372036854775808"), nlohmann::json("-9223372036854775809"),
         nlohmann::json::object(), nlohmann::json::array(), nlohmann::json {{"value", 1}}}) {
    SCOPED_TRACE(invalid.dump());
    memory_state_store_t store;
    auto damaged = nlohmann::json::parse(apollo_snapshot);
    damaged["root"]["named_devices"][0]["last_seen"] = invalid;
    ASSERT_FALSE(nvhttp::state_policy::valid_primary_json(damaged.dump()));
    store.files["shared.json"] = damaged.dump();
    store.files["shared.json.bak"] = backup.dump();
    const auto original = store.files;
    pt::ptree tree;
    EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(store.files, original);

    store.files["shared.json"] = "broken";
    store.files["shared.json.bak"] = damaged.dump();
    EXPECT_EQ(load_apollo_primary(store, tree), policy::load_result_e::failed);
    EXPECT_EQ(store.files["shared.json"], "broken");
    EXPECT_EQ(store.files["shared.json.bak"], damaged.dump());
  }
}

TEST(StateStorageApollo, MalformedPropertyTreeTimestampCannotBePublished) {
  const auto backup = apollo_tree(nlohmann::json::parse(apollo_snapshot));
  for (const auto invalid : {"", "bad", "true", "1tail", " 1", "1 ", "+1", "01", "1.0", "1e3", "9223372036854775808", "-9223372036854775809"}) {
    SCOPED_TRACE(invalid);
    auto tree = backup;
    tree.get_child("root.named_devices").front().second.put("last_seen", invalid);
    EXPECT_FALSE(policy::primary_write_allowed(tree, policy::load_result_e::loaded, backup, nvhttp::state_policy::valid_primary_tree));
  }
  auto tree = backup;
  auto &timestamp = tree.get_child("root.named_devices").front().second.put("last_seen", "1");
  timestamp.put("nested", "1");
  EXPECT_FALSE(policy::primary_write_allowed(tree, policy::load_result_e::loaded, backup, nvhttp::state_policy::valid_primary_tree));
}

TEST(StateStorageApollo, MissingReadCallbackFailsClosed) {
  pt::ptree tree;
  tree.put("old", "value");
  EXPECT_EQ(policy::load_primary_state_for_update("shared.json", tree, {}, {}, nvhttp::state_policy::valid_primary_tree,
    nvhttp::state_policy::valid_primary_json), policy::load_result_e::failed);
  EXPECT_TRUE(tree.empty());
}

TEST(StateStorageWrite, LinuxDisplayBaselineSurvivesAtomicAuxiliaryRoundTrip) {
  memory_state_store_t store;
  const std::string snapshot = R"({"version":1,"owner":"1000:desktop","topology":{"outputs":[{"name":"eDP-1","connected":true,"enabled":false},{"name":"DP-1","connected":true,"enabled":true}]}})";
  pt::ptree tree;
  tree.put("root.linux_display_topology", snapshot);
  tree.put("root.unrelated_setting", "preserved");
  policy::write_vibeshine_state("aux.json", tree, [&store](const std::string &path, const std::string &contents) {
    return store.write(path, contents);
  },
                                [&store](const std::string &path) {
                                  return store.read(path);
                                });
  ASSERT_TRUE(store.files.contains("aux.json.bak"));
  // A corrupt main state recovers the saved topology with its original types
  // intact in the serialized payload, while retaining unrelated machine state.
  store.files["aux.json"] = "{";
  pt::ptree recovered;
  ASSERT_EQ(policy::load_vibeshine_state("aux.json", recovered, [&store](const std::string &path) {
              return store.read(path);
            },
                                         [&store](const std::string &path, const std::string &contents) {
                                           return store.write(path, contents);
                                         }),
            policy::load_result_e::loaded);
  EXPECT_EQ(recovered.get<std::string>("root.linux_display_topology"), snapshot);
  EXPECT_EQ(recovered.get<std::string>("root.unrelated_setting"), "preserved");
}

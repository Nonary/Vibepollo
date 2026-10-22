/** Offline recovery uses only disposable snapshots, never a host profile. */
#include "../tests_common.h"

#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <src/crypto.h>
#include <src/http_pairing_policy.h>
#include <src/pairing_recovery.h>
#include <src/paired_state_policy.h>
#include <src/state_storage_policy.h>
#include <sstream>

#ifdef _WIN32
  #include <Windows.h>
#else
  #include <csignal>
  #include <sys/resource.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {
  namespace fs = std::filesystem;
  using json = nlohmann::json;
  constexpr auto first_uuid = "22222222-2222-2222-2222-222222222221";
  constexpr auto second_uuid = "22222222-2222-2222-2222-222222222222";
  constexpr auto third_uuid = "22222222-2222-2222-2222-222222222223";

  std::string certificate(long serial) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw_key = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 || EVP_PKEY_keygen(context.get(), &raw_key) <= 0) {
      throw std::runtime_error("test key generation failed");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400);
    X509_set_pubkey(cert.get(), key.get());
    auto *name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("synthetic recovery fixture"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
      throw std::runtime_error("test signing failed");
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_X509(output.get(), cert.get());
    char *data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    return {data, static_cast<std::size_t>(size)};
  }

  std::string read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }

  void write(const fs::path &path, const std::string &bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
  }

  std::optional<std::string> parsed_identity(const std::string &pem) {
    auto certificate = crypto::x509(pem);
    return certificate ? std::optional {crypto::pem(certificate)} : std::nullopt;
  }

  std::string digest(const std::string &bytes) {
    std::ostringstream result;
    for (const auto byte : crypto::hash(bytes)) {
      result << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    }
    return result.str();
  }

  // Data adapter only. Authorization decisions execute http_pairing_policy.cpp;
  // certificate parsing/encoding executes crypto.cpp, not a test replacement.
  nvhttp::pairing_policy::paired_client_resolution_e authorization(const json &state, const std::string &presented) {
    const auto &named = state["root"]["named_devices"];
    std::vector<std::string> identities;
    identities.reserve(named.size());
    std::vector<nvhttp::pairing_policy::paired_client_record_view_t> records;
    for (const auto &record : named) {
      identities.push_back(parsed_identity(record["cert"].get<std::string>()).value());
      const auto permissions = static_cast<crypto::PERM>(util::get_non_string_json_value<std::uint32_t>(
        record, "perm", static_cast<std::uint32_t>(crypto::PERM::_all))) & crypto::PERM::_all;
      records.push_back({record["uuid"].get_ref<const std::string &>(), identities.back(), util::get_non_string_json_value<bool>(record, "enabled", true) && !!permissions});
    }
    return nvhttp::pairing_policy::resolve_paired_client(records, presented).status;
  }
}  // namespace

class PairingRecovery: public testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence {0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = fs::temp_directory_path() / ("pairing-recovery-test-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
    ASSERT_TRUE(fs::create_directory(directory));
    input = directory / "snapshot.json";
    output = directory / "candidate";
    static const auto duplicate_cert = certificate(1);
    static const auto other_cert = certificate(0);  // Legacy serial zero remains parseable.
    state = {
      {"username", "current-owner"},
      {"password", "current-hash"},
      {"salt", "current-salt"},
      {"unknown_top_level", {{"array", {true, 7, nullptr}}, {"string", "unchanged"}}},
      {"root", {{"uniqueid", "11111111-1111-1111-1111-111111111111"}, {"api_tokens", {{{"hash", "current-token"}}}}, {"named_devices", {{{"uuid", first_uuid}, {"name", "first"}, {"cert", duplicate_cert}, {"enabled", "true"}, {"last_seen", "1790870400"}, {"config_overrides", {{"output_name", "first-output"}}}, {"unknown", {1, 2}}}, {{"uuid", second_uuid}, {"name", "second"}, {"cert", duplicate_cert}, {"enabled", "true"}, {"config_overrides", {{"output_name", "second-output"}}}}, {{"uuid", third_uuid}, {"name", "unrelated serial-zero"}, {"cert", other_cert}, {"enabled", false}}}}}}
    };
    save();
    write(input.string() + ".bak", "{\"password\":\"stale-backup-secret\"}");
  }

  void TearDown() override {
    fs::remove_all(directory);
  }

  void save() {
    write(input, state.dump(2) + "\n");
  }

  int invoke(std::vector<std::string> arguments) {
    stdout_stream.str({});
    stderr_stream.str({});
    std::vector<const char *> argv;
    for (const auto &argument : arguments) {
      argv.push_back(argument.c_str());
    }
    return pairing_recovery::run(static_cast<int>(argv.size()), argv.data(), stdout_stream, stderr_stream);
  }

  std::optional<int> dispatch(std::vector<std::string> arguments) {
    stdout_stream.str({});
    stderr_stream.str({});
    std::vector<const char *> argv;
    for (const auto &argument : arguments) {
      argv.push_back(argument.c_str());
    }
    return pairing_recovery::dispatch(static_cast<int>(argv.size()), argv.data(), stdout_stream, stderr_stream);
  }

  std::string token() {
    EXPECT_EQ(invoke({"inspect", input.string()}), 0) << stderr_stream.str();
    return json::parse(stdout_stream.str()).at("snapshot_token");
  }

  int prepare(const std::vector<std::string> &keep, std::string snapshot = {}) {
    if (snapshot.empty()) {
      snapshot = token();
    }
    std::vector<std::string> arguments {"prepare", input.string(), "--expect-snapshot", snapshot, "--output", output.string()};
    for (const auto &uuid : keep) {
      arguments.push_back("--keep");
      arguments.push_back(uuid);
    }
    return invoke(std::move(arguments));
  }

  fs::path directory, input, output;
  json state;
  std::ostringstream stdout_stream, stderr_stream;
};

TEST_F(PairingRecovery, InspectionIsReadOnlyAndDoesNotDiscloseCredentialsOrCertificates) {
  const auto primary = read(input), backup = read(input.string() + ".bak");
  ASSERT_EQ(invoke({"inspect", input.string()}), 0);
  const auto report = json::parse(stdout_stream.str());
  ASSERT_EQ(report["duplicate_groups"].size(), 1u);
  EXPECT_EQ(report["duplicate_groups"][0]["records"][0]["uuid"], first_uuid);
  for (const auto secret : {"current-hash", "current-salt", "current-token", "stale-backup-secret", "BEGIN CERTIFICATE"}) {
    EXPECT_EQ(stdout_stream.str().find(secret), std::string::npos);
  }
  EXPECT_EQ(read(input), primary);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PairingRecovery, ExplicitSurvivorPreservesItsWholeRecordAndAllUnrelatedState) {
  const auto primary = read(input), backup = read(input.string() + ".bak");
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  auto expected = state;
  expected["root"]["named_devices"].erase(1);
  EXPECT_EQ(json::parse(read(output / "sunshine_state.reconciled.json")), expected);
  EXPECT_EQ(read(output / "primary.original.json"), primary);
  EXPECT_EQ(read(output / "backup.original.json"), backup);
  EXPECT_EQ(read(input), primary);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  EXPECT_TRUE(json::parse(read(output / "recovery.json"))["complete"]);
#ifndef _WIN32
  EXPECT_EQ(fs::status(output).permissions() & fs::perms::all, fs::perms::owner_all);
  EXPECT_EQ(fs::status(output / "primary.original.json").permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
#endif
}

TEST_F(PairingRecovery, NativeRevokedMasksPreventPermissiveSelection) {
  const auto original = state;
  for (const auto &revoked_mask : json::array({0, "0", static_cast<std::uint32_t>(crypto::PERM::_reserved)})) {
    state = original;
    auto &records = state["root"]["named_devices"];
    records[0].erase("enabled");
    records[1].erase("enabled");
    records[0]["perm"] = static_cast<std::uint32_t>(crypto::PERM::_all);
    records[1]["perm"] = revoked_mask;
    save();
    ASSERT_EQ(invoke({"inspect", input.string()}), 0);
    const auto report = json::parse(stdout_stream.str());
    EXPECT_EQ(report["duplicate_groups"][0]["records"][0]["enabled"], true);
    EXPECT_EQ(report["duplicate_groups"][0]["records"][1]["enabled"], false);
    EXPECT_EQ(prepare({first_uuid}), 2);
    EXPECT_FALSE(fs::exists(output));
    ASSERT_EQ(prepare({second_uuid}), 0) << stderr_stream.str();
    const auto candidate = json::parse(read(output / "sunshine_state.reconciled.json"));
    EXPECT_EQ(candidate["root"]["named_devices"][0]["perm"], revoked_mask);
    EXPECT_EQ(authorization(candidate, parsed_identity(records[1]["cert"]).value()),
              nvhttp::pairing_policy::paired_client_resolution_e::disabled);
    fs::remove_all(output);
  }
}

TEST_F(PairingRecovery, LegacyRevocationOverridesNativePermissionMask) {
  auto &records = state["root"]["named_devices"];
  records[0]["enabled"] = false;
  records[0]["perm"] = static_cast<std::uint32_t>(crypto::PERM::_all);
  records[1]["perm"] = static_cast<std::uint32_t>(crypto::PERM::_all);
  save();
  EXPECT_EQ(prepare({second_uuid}), 2);
  EXPECT_FALSE(fs::exists(output));
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  const auto candidate = json::parse(read(output / "sunshine_state.reconciled.json"));
  EXPECT_EQ(candidate["root"]["named_devices"][0], records[0]);
  EXPECT_EQ(authorization(candidate, parsed_identity(records[0]["cert"]).value()),
            nvhttp::pairing_policy::paired_client_resolution_e::disabled);
}

TEST_F(PairingRecovery, NativePermissionMaskAndPreferencesRemainExact) {
  auto &records = state["root"]["named_devices"];
  records[0].erase("enabled");
  records[1].erase("enabled");
  records[0]["perm"] = std::to_string(static_cast<std::uint32_t>(crypto::PERM::input_kbd));
  records[1]["perm"] = static_cast<std::uint32_t>(crypto::PERM::_default);
  records[0]["virtual_display_layout"] = "extended_primary_isolated";
  records[0]["allow_client_commands"] = false;
  records[0]["hdr_profile"] = "retained-client-profile";
  save();
  const auto before = read(input);
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  auto expected = state;
  expected["root"]["named_devices"].erase(1);
  const auto candidate = json::parse(read(output / "sunshine_state.reconciled.json"));
  EXPECT_EQ(candidate, expected);
  EXPECT_EQ(read(input), before);
  EXPECT_EQ(authorization(candidate, parsed_identity(records[0]["cert"]).value()),
            nvhttp::pairing_policy::paired_client_resolution_e::authorized);
}

TEST_F(PairingRecovery, MalformedNativePermissionsNeverProduceCandidates) {
  const auto original = state;
  for (const auto &mask : json::array({"not-a-number", "8junk", "4294967296", "-1", -1, 4294967296ULL, 1.5, true, nullptr})) {
    SCOPED_TRACE(mask.dump());
    state = original;
    state["root"]["named_devices"][0]["perm"] = mask;
    save();
    const auto primary = read(input), backup = read(input.string() + ".bak");
    const auto snapshot = digest(digest(primary) + ":" + digest(backup));
    EXPECT_EQ(invoke({"inspect", input.string()}), 2);
    EXPECT_EQ(prepare({first_uuid}, snapshot), 2);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(read(input), primary);
    EXPECT_EQ(read(input.string() + ".bak"), backup);
    fs::remove_all(output);
  }
}

TEST_F(PairingRecovery, NativeDuplicateToleranceIsExplicitAndKeepsTypedValidation) {
  auto strict = state;
  std::vector<statefile::policy::collapsed_pairing_record_t> collapsed;
  ASSERT_TRUE(nvhttp::state_policy::normalize_snapshot(strict, parsed_identity, false, &collapsed));
  ASSERT_EQ(collapsed.size(), 1u);
  EXPECT_EQ(collapsed[0].kept_uuid, first_uuid);
  EXPECT_EQ(collapsed[0].dropped_uuid, second_uuid);
  ASSERT_EQ(strict["root"]["named_devices"].size(), 2u);
  EXPECT_EQ(strict["root"]["named_devices"][0]["name"], "first");
  auto inspected = state;
  EXPECT_TRUE(nvhttp::state_policy::normalize_snapshot(inspected, parsed_identity, true));
  inspected = state;
  inspected["root"]["named_devices"][1]["perm"] = "8junk";
  EXPECT_FALSE(nvhttp::state_policy::normalize_snapshot(inspected, parsed_identity, true));
  inspected = state;
  inspected["root"]["named_devices"][1]["uuid"] = first_uuid;
  EXPECT_FALSE(nvhttp::state_policy::normalize_snapshot(inspected, parsed_identity, true));
}

TEST_F(PairingRecovery, NativeRejectedBooleanSpellingsNeverProduceCandidates) {
  const auto original = state;
  for (const auto &enabled : json::array({"false ", " true", "no", "yes", 2})) {
    SCOPED_TRACE(enabled.dump());
    state = original;
    state["root"]["named_devices"][0]["enabled"] = enabled;
    state["root"]["named_devices"][1]["perm"] = 0;
    save();
    const auto primary = read(input), backup = read(input.string() + ".bak");
    const auto snapshot = digest(digest(primary) + ":" + digest(backup));
    EXPECT_EQ(invoke({"inspect", input.string()}), 2);
    EXPECT_EQ(prepare({first_uuid}, snapshot), 2);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(read(input), primary);
    EXPECT_EQ(read(input.string() + ".bak"), backup);
    fs::remove_all(output);
  }
}

TEST_F(PairingRecovery, EveryRevocationCombinationPreventsPermissiveSelection) {
  for (const bool first : {false, true}) {
    for (const bool second : {false, true}) {
      for (const auto selected : {first_uuid, second_uuid}) {
        state["root"]["named_devices"][0]["enabled"] = first ? "true" : "false";
        state["root"]["named_devices"][1]["enabled"] = second;  // Both scalar encodings.
        save();
        const bool enabled = selected == first_uuid ? first : second;
        const bool safe = (first && second) || !enabled;
        EXPECT_EQ(prepare({selected}), safe ? 0 : 2) << stderr_stream.str();
        EXPECT_EQ(fs::exists(output), safe);
        if (safe) {
          const auto result = json::parse(read(output / "sunshine_state.reconciled.json"));
          EXPECT_EQ(result["root"]["named_devices"][0], state["root"]["named_devices"][selected == first_uuid ? 0 : 1]);
          fs::remove_all(output);
        }
      }
    }
  }
}

TEST_F(PairingRecovery, EveryAmbiguousGroupNeedsExactlyOneExistingUuid) {
  for (const auto &keep : std::vector<std::vector<std::string>> {{}, {first_uuid, second_uuid}, {third_uuid}, {first_uuid, first_uuid}}) {
    EXPECT_EQ(prepare(keep), 2);
    EXPECT_FALSE(fs::exists(output));
  }
}

TEST_F(PairingRecovery, ExactSameUuidCopiesAreLosslessButDifferentRecordsAreRejected) {
  state["root"]["named_devices"][1] = state["root"]["named_devices"][0];
  save();
  ASSERT_EQ(prepare({}), 0) << stderr_stream.str();
  auto expected = state;
  expected["root"]["named_devices"].erase(1);
  EXPECT_EQ(json::parse(read(output / "sunshine_state.reconciled.json")), expected);
  fs::remove_all(output);
  state["root"]["named_devices"][1]["enabled"] = "false";
  save();
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PairingRecovery, SnapshotTokenBindsPrimaryAndBackupIncludingMissingBackup) {
  auto inspected = token();
  state["password"] = "newer-secret";
  save();
  EXPECT_EQ(prepare({first_uuid}, inspected), 2);
  EXPECT_FALSE(fs::exists(output));
  inspected = token();
  write(input.string() + ".bak", "new backup bytes");
  EXPECT_EQ(prepare({first_uuid}, inspected), 2);
  EXPECT_FALSE(fs::exists(output));
  inspected = token();
  fs::remove(input.string() + ".bak");
  EXPECT_EQ(prepare({first_uuid}, inspected), 2);
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  EXPECT_FALSE(fs::exists(output / "backup.original.json"));
}

TEST_F(PairingRecovery, ValidDivergentBackupIsPreservedButNeverSelectedOrMerged) {
  const auto primary = read(input);
  auto backup_state = state;
  backup_state["username"] = "backup-owner";
  backup_state["password"] = "backup-password-hash";
  backup_state["salt"] = "backup-salt";
  auto backup_only = state["root"]["named_devices"][2];
  backup_only["uuid"] = "22222222-2222-2222-2222-222222222224";
  backup_only["name"] = "backup-only-client";
  backup_only["enabled"] = "true";
  backup_only["config_overrides"]["output_name"] = "backup-output";
  backup_state["root"]["named_devices"] = json::array({backup_only});
  const auto backup = backup_state.dump(2) + "\n";
  boost::property_tree::ptree validated_backup;
  std::istringstream backup_stream(backup);
  boost::property_tree::read_json(backup_stream, validated_backup);
  ASSERT_TRUE(statefile::policy::valid_primary_state(validated_backup, false));
  write(input.string() + ".bak", backup);

  ASSERT_EQ(invoke({"inspect", input.string()}), 0) << stderr_stream.str();
  const auto inspection = json::parse(stdout_stream.str());
  ASSERT_EQ(inspection["duplicate_groups"].size(), 1u);
  ASSERT_EQ(inspection["duplicate_groups"][0]["records"].size(), 2u);
  EXPECT_EQ(inspection["duplicate_groups"][0]["records"][0]["uuid"], first_uuid);
  EXPECT_EQ(inspection["duplicate_groups"][0]["records"][1]["uuid"], second_uuid);
  EXPECT_EQ(inspection["backup_sha256"], digest(backup));

  ASSERT_EQ(prepare({first_uuid}, inspection["snapshot_token"].get<std::string>()), 0) << stderr_stream.str();
  auto expected = state;
  expected["root"]["named_devices"].erase(1);
  const auto candidate = read(output / "sunshine_state.reconciled.json");
  EXPECT_EQ(json::parse(candidate), expected);
  EXPECT_EQ(candidate.find("backup-only-client"), std::string::npos);
  EXPECT_EQ(candidate.find("backup-owner"), std::string::npos);
  EXPECT_EQ(read(output / "primary.original.json"), primary);
  EXPECT_EQ(read(output / "backup.original.json"), backup);
  EXPECT_EQ(read(input), primary);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
}

TEST_F(PairingRecovery, ExistingOutputAndLinksCannotOverwriteOriginalSnapshots) {
  ASSERT_TRUE(fs::create_directory(output));
  write(output / "sentinel", "preserve");
  EXPECT_EQ(prepare({first_uuid}), 2);
  EXPECT_EQ(read(output / "sentinel"), "preserve");
#ifndef _WIN32
  const auto link = directory / "link.json";
  fs::create_symlink(input, link);
  EXPECT_EQ(invoke({"inspect", link.string()}), 2);
#endif
}

TEST_F(PairingRecovery, InvalidCertificatesAndDuplicateKeysFailWithoutEchoingSecrets) {
  state["root"]["named_devices"][0]["cert"] = "secret-invalid-certificate";
  save();
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_EQ(stderr_stream.str().find("secret-invalid-certificate"), std::string::npos);
  write(input, "{\"password\":\"secret-one\",\"password\":\"secret-two\"}");
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_EQ(stderr_stream.str().find("secret-one"), std::string::npos);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PairingRecovery, LegacyAliasesAndRepeatedLegacyCertificatesArePreservedInTheCandidate) {
  const auto cert = state["root"]["named_devices"][0]["cert"];
  state["root"]["devices"] = {{{"uniqueid", "legacy"}, {"certs", {cert}}}};
  save();
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  EXPECT_EQ(json::parse(read(output / "sunshine_state.reconciled.json"))["root"]["devices"], state["root"]["devices"]);
  fs::remove_all(output);
  // Startup collapses a repeated legacy-only certificate losslessly, so it no
  // longer blocks a candidate. This named-client operation leaves it untouched.
  state["root"]["named_devices"].erase(2);
  const auto legacy_cert = certificate(2);
  state["root"]["devices"][0]["certs"] = {legacy_cert, legacy_cert};
  save();
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  EXPECT_EQ(json::parse(read(output / "sunshine_state.reconciled.json"))["root"]["devices"], state["root"]["devices"]);
}

TEST_F(PairingRecovery, BoundedInputAndNestingAreEnforcedBeforeOutput) {
  write(input, std::string(16 * 1024 * 1024 + 1, ' '));
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  write(input, std::string(40, '[') + "0" + std::string(40, ']'));
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PairingRecovery, UnrelatedNumberLexemesAndEscapedKeysSurviveExactly) {
  state["root"]["named_devices"][0]["unknown_number"] = "NUMBER-LEXEME";
  state["unknown_top_level"]["float"] = "FLOAT-LEXEME";
  auto original = state.dump(2);
  original.replace(original.find("\"NUMBER-LEXEME\""), 15, "1234567890123456789012345678901234567890");
  original.replace(original.find("\"FLOAT-LEXEME\""), 14, "0.12345678901234567890123456789");
  original.replace(original.find("\"root\""), 6, "\"r\\u006fot\"");
  original.replace(original.find("\"named_devices\""), 15, "\"named_\\u0064evices\"");
  write(input, "\xEF\xBB\xBF" + original + "\n");
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  const auto candidate = read(output / "sunshine_state.reconciled.json");
  EXPECT_TRUE(candidate.starts_with("\xEF\xBB\xBF"));
  EXPECT_NE(candidate.find("1234567890123456789012345678901234567890"), std::string::npos);
  EXPECT_NE(candidate.find("0.12345678901234567890123456789"), std::string::npos);
  EXPECT_NE(candidate.find("\"r\\u006fot\""), std::string::npos);
  EXPECT_EQ(read(output / "primary.original.json"), "\xEF\xBB\xBF" + original + "\n");
}

TEST_F(PairingRecovery, MultipleGroupsCanRemoveFirstAndLastRecordsWithoutChangingSurvivors) {
  auto fourth = state["root"]["named_devices"][2];
  fourth["uuid"] = "22222222-2222-2222-2222-222222222224";
  fourth["name"] = "fourth";
  state["root"]["named_devices"].push_back(fourth);
  save();
  ASSERT_EQ(prepare({second_uuid, third_uuid}), 0) << stderr_stream.str();
  auto expected = state;
  expected["root"]["named_devices"].erase(3);
  expected["root"]["named_devices"].erase(0);
  EXPECT_EQ(json::parse(read(output / "sunshine_state.reconciled.json")), expected);
}

TEST_F(PairingRecovery, SameUuidRecordsWithDifferentNumericLexemesCannotCollapse) {
  state["root"]["named_devices"][0]["unknown_number"] = "ONE";
  state["root"]["named_devices"][1] = state["root"]["named_devices"][0];
  state["root"]["named_devices"][1]["unknown_number"] = "OTHER";
  auto bytes = state.dump();
  bytes.replace(bytes.find("\"ONE\""), 5, "1");
  bytes.replace(bytes.find("\"OTHER\""), 7, "1.0");
  write(input, bytes);
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PairingRecovery, ProductionDispatcherAdmitsOnlyAnExplicitFirstFlag) {
  const auto original = read(input), backup = read(input.string() + ".bak");
  for (const auto &arguments : std::vector<std::vector<std::string>> {
         {},
         {"vibeshine"},
         {"vibeshine", "--version"},
         {"vibeshine", "--other", "--pairing-recovery", "inspect", input.string()},
         {"vibeshine", "--pairing-recovery=inspect", input.string()},
         {"vibeshine", "--PAIRING-RECOVERY", "inspect", input.string()}
       }) {
    EXPECT_FALSE(dispatch(arguments).has_value());
    EXPECT_TRUE(stdout_stream.str().empty());
    EXPECT_TRUE(stderr_stream.str().empty());
  }
  ASSERT_EQ(dispatch({"vibeshine", "--pairing-recovery", "inspect", input.string()}), 0);
  const auto inspected = json::parse(stdout_stream.str());
  ASSERT_EQ(dispatch({"vibeshine", "--pairing-recovery", "prepare", input.string(), "--expect-snapshot", inspected["snapshot_token"].get<std::string>(), "--keep", first_uuid, "--output", output.string()}), 0)
    << stderr_stream.str();
  EXPECT_TRUE(fs::exists(output / "recovery.json"));
  EXPECT_EQ(read(input), original);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
}

TEST_F(PairingRecovery, ProductionCommandRejectsIncompleteUnknownAndRepeatedOptions) {
  const auto original = read(input), backup = read(input.string() + ".bak");
  const auto inspected = token();
  const std::vector<std::vector<std::string>> invalid {
    {},
    {"inspect"},
    {"automatic-repair", input.string()},
    {"inspect", input.string(), "--output", output.string()},
    {"inspect", input.string(), "unexpected-argument"},
    {"prepare", input.string(), "--output"},
    {"prepare", input.string(), "--expect-snapshot", inspected, "--keep", first_uuid},
    {"prepare", input.string(), "--keep", first_uuid, "--output", output.string()},
    {"prepare", input.string(), "--expect-snapshot", inspected, "--keep", first_uuid, "--output", output.string(), "--output", output.string()},
    {"prepare", input.string(), "--expect-snapshot", inspected, "--expect-snapshot", inspected, "--keep", first_uuid, "--output", output.string()},
    {"prepare", input.string(), "--expect-snapshot", inspected, "--keep", first_uuid, "--output", output.string(), "--force", "true"}
  };
  for (const auto &arguments : invalid) {
    EXPECT_EQ(invoke(arguments), 2);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(read(input), original);
    EXPECT_EQ(read(input.string() + ".bak"), backup);
  }
  EXPECT_EQ(dispatch({"vibeshine", "--pairing-recovery"}), 2);
}

TEST_F(PairingRecovery, MalformedAndChangedSnapshotTokensCannotOverrideNewRevocations) {
  const auto inspected = token();
  state["root"]["named_devices"][0]["enabled"] = false;
  save();
  const auto revoked = read(input), backup = read(input.string() + ".bak");
  for (const auto &token : {inspected, std::string("not-a-hash"), std::string(64, '0')}) {
    EXPECT_EQ(prepare({second_uuid}, token), 2);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_EQ(read(input), revoked);
    EXPECT_EQ(read(input.string() + ".bak"), backup);
  }
  fs::remove(input.string() + ".bak");
  const auto no_backup = token();
  write(input.string() + ".bak", backup);
  EXPECT_EQ(prepare({first_uuid}, no_backup), 2);
  EXPECT_FALSE(fs::exists(output));
  EXPECT_EQ(read(input), revoked);
}

TEST_F(PairingRecovery, ManifestHashesBindExactSourceBackupAndCandidateBytes) {
  const auto original = read(input), backup = read(input.string() + ".bak");
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  const auto manifest = json::parse(read(output / "recovery.json"));
  const auto candidate = read(output / "sunshine_state.reconciled.json");
  EXPECT_EQ(manifest["primary_sha256"], digest(original));
  EXPECT_EQ(manifest["backup_sha256"], digest(backup));
  EXPECT_EQ(manifest["candidate_sha256"], digest(candidate));
  EXPECT_EQ(manifest["snapshot_token"], digest(digest(original) + ":" + digest(backup)));
  EXPECT_EQ(manifest["selected_uuids"], json::array({first_uuid}));
}

TEST_F(PairingRecovery, CandidateIsAnExactSourceSpliceRatherThanAJsonRoundTrip) {
  const std::string prefix =
    "\xEF\xBB\xBF{\r\n\"password\":\"current-hash\",\"username\":\"owner\",\"salt\":\"salt\","
    "\"unknown\":{\"huge\":1234567890123456789012345678901234567890,\"float\":0.12345678901234567890123456789},"
    "\"r\\u006fot\":{\"uniqueid\":\"11111111-1111-1111-1111-111111111111\",\"named_\\u0064evices\":[\r\n\t";
  const std::string separator = ", \r\n\t";
  const std::string suffix = "\r\n],\"unknown_root\":{\"escaped\":\"\\\"}][\\\\\",\"nil\":null,\"array\":[{},[],true]}}\r\n}\r\n";
  const auto first = state["root"]["named_devices"][0].dump(2);
  const auto second = state["root"]["named_devices"][1].dump(2);
  const auto third = state["root"]["named_devices"][2].dump(2);
  const auto original = prefix + first + separator + second + separator + third + suffix;
  write(input, original);
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  EXPECT_EQ(read(output / "sunshine_state.reconciled.json"), prefix + first + separator + third + suffix);
  EXPECT_EQ(read(output / "primary.original.json"), original);
  EXPECT_EQ(read(input), original);
}

TEST_F(PairingRecovery, StartupCollapseMatchesAnExplicitFirstRecordCandidate) {
  namespace policy = statefile::policy;
  namespace authorization_policy = nvhttp::pairing_policy;
  const auto original = read(input);
  boost::property_tree::ptree before;
  std::istringstream initial(original);
  boost::property_tree::read_json(initial, before);
  ASSERT_TRUE(policy::valid_primary_state(before, false));
  // The raw shape is ambiguous to authorization, so startup normalizes it first.
  // No copied validator or fake certificate identities: crypto.cpp parses the
  // actual test PEMs.
  const auto presented = parsed_identity(state["root"]["named_devices"][0]["cert"].get<std::string>()).value();
  EXPECT_EQ(authorization(state, presented), authorization_policy::paired_client_resolution_e::invalid_state);
  ASSERT_TRUE(policy::normalize_legacy_pairing_state(before, parsed_identity));
  EXPECT_TRUE(policy::valid_primary_state(before, false));
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  const auto candidate_bytes = read(output / "sunshine_state.reconciled.json");
  boost::property_tree::ptree after;
  std::istringstream candidate(candidate_bytes);
  boost::property_tree::read_json(candidate, after);
  EXPECT_EQ(before, after);
  EXPECT_TRUE(policy::normalize_legacy_pairing_state(after, parsed_identity));
  EXPECT_EQ(authorization(json::parse(candidate_bytes), presented), authorization_policy::paired_client_resolution_e::authorized);
  EXPECT_EQ(read(input), original);  // Recovery never changes its input.
}

TEST_F(PairingRecovery, SelectingARevokedSurvivorStillDeniesItsCertificateThroughProductionAuthorization) {
  namespace policy = nvhttp::pairing_policy;
  state["root"]["named_devices"][0]["enabled"] = false;
  state["root"]["named_devices"][1]["enabled"] = "true";
  save();
  const auto original = read(input), backup = read(input.string() + ".bak");
  EXPECT_EQ(prepare({second_uuid}), 2);
  EXPECT_FALSE(fs::exists(output));
  EXPECT_EQ(read(input), original);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  ASSERT_EQ(prepare({first_uuid}), 0) << stderr_stream.str();
  const auto recovered = json::parse(read(output / "sunshine_state.reconciled.json"));
  const auto presented = parsed_identity(state["root"]["named_devices"][0]["cert"].get<std::string>()).value();
  EXPECT_EQ(authorization(recovered, presented), policy::paired_client_resolution_e::disabled);
  EXPECT_EQ(recovered["root"]["named_devices"][0], state["root"]["named_devices"][0]);
  EXPECT_EQ(read(input), original);
}

TEST_F(PairingRecovery, RealInputErrorsNeverCreateCandidateOrChangeOriginals) {
  const auto original = read(input), backup = read(input.string() + ".bak");
  EXPECT_EQ(invoke({"inspect", (directory / "missing.json").string()}), 2);
  EXPECT_EQ(invoke({"inspect", directory.string()}), 2);
  EXPECT_FALSE(fs::exists(output));
#ifndef _WIN32
  const auto fifo = directory / "fifo.json";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_EQ(invoke({"inspect", fifo.string()}), 2);
  fs::remove(input.string() + ".bak");
  fs::create_symlink(directory / "missing-target", input.string() + ".bak");
  EXPECT_EQ(invoke({"inspect", input.string()}), 2);
  EXPECT_FALSE(fs::exists(output));
  fs::remove(input.string() + ".bak");
  write(input.string() + ".bak", backup);
#endif
  EXPECT_EQ(read(input), original);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
}

#ifndef _WIN32
TEST_F(PairingRecovery, RealPartialWriteFailureLeavesOriginalsAndNoCompleteManifest) {
  const auto original = read(input);
  const std::string backup(original.size() * 2, 'B');
  write(input.string() + ".bak", backup);
  const auto inspected = token();
  // OS-only failure injection in a child. Recovery, parser, validation, writes
  // and exception handling are the real compiled production implementation.
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_FSIZE, &limit) != 0 || original.size() > limit.rlim_max) {
      std::_Exit(125);
    }
    limit.rlim_cur = original.size();
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      std::_Exit(125);
    }
    const auto result = prepare({first_uuid}, inspected);
    std::_Exit(result == 2 && stderr_stream.str().find("incomplete output") != std::string::npos ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(read(input), original);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  EXPECT_EQ(read(output / "primary.original.json"), original);
  EXPECT_LT(fs::file_size(output / "backup.original.json"), backup.size());
  EXPECT_FALSE(fs::exists(output / "sunshine_state.reconciled.json"));
  EXPECT_FALSE(fs::exists(output / "recovery.json"));
}
#endif

class AuxiliaryRecovery: public PairingRecovery {
protected:
  void SetUp() override {
    PairingRecovery::SetUp();
    input = directory / "vibeshine_state.json";
    write(input, std::string(512, '\0'));
    write(input.string() + ".bak", std::string(256, '\0'));
  }

  std::optional<int> inspect() {
    return dispatch({"sunshine", "--auxiliary-recovery", "inspect", input.string()});
  }

  std::string auxiliary_token() {
    EXPECT_EQ(inspect(), 0) << stderr_stream.str();
    return json::parse(stdout_stream.str()).at("snapshot_token");
  }

  std::optional<int> reset(const std::string &token) {
    return dispatch({"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--expect-snapshot", token, "--accept-token-and-metadata-loss", "--confirm-separate-auxiliary-state", "--output", output.string()});
  }
};

TEST_F(AuxiliaryRecovery, ExplicitResetPreservesOriginalsAndCannotResurrectTokens) {
  const auto pairing = state.dump();
  write(directory / "sunshine_state.json", pairing);
  const auto primary = read(input), backup = read(input.string() + ".bak");
  const auto inspected = auxiliary_token();
  EXPECT_FALSE(fs::exists(output));
  ASSERT_EQ(reset(inspected), 0) << stderr_stream.str();
  EXPECT_EQ(read(input), primary);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  EXPECT_EQ(read(output / "primary.original.json"), primary);
  EXPECT_EQ(read(output / "backup.original.json"), backup);
  EXPECT_EQ(read(directory / "sunshine_state.json"), pairing);
  const auto candidate = read(output / "vibeshine_state.reset.json");
  EXPECT_EQ(read(output / "vibeshine_state.reset.json.bak"), candidate);
  const auto state = json::parse(candidate);
  EXPECT_EQ(state["root"]["api_tokens"], json::array());
  EXPECT_EQ(state["root"]["session_tokens"], json::array());
  EXPECT_EQ(state.size(), 1u);
  EXPECT_EQ(state["root"].size(), 2u);
  EXPECT_FALSE(fs::exists(output / "sunshine_state.reconciled.json"));
  const auto report = json::parse(read(output / "recovery.json"));
  EXPECT_EQ(report["operation"], "auxiliary_reset_not_recovery");
  EXPECT_EQ(report["complete"], true);
  EXPECT_EQ(report["operator_confirmed_separate_auxiliary_state"], true);
  EXPECT_EQ(report["candidate_sha256"], digest(candidate));
  boost::property_tree::ptree loaded;
  EXPECT_EQ(statefile::policy::load_vibeshine_state("auxiliary", loaded, [&](const std::string &) {
              return statefile::policy::read_result_t {statefile::policy::read_status_e::loaded, candidate};
            },
                                                    [](const std::string &, const std::string &) {
                                                      ADD_FAILURE() << "Validated identical reset copies should not be rewritten";
                                                      return false;
                                                    }),
            statefile::policy::load_result_e::loaded);
}

TEST_F(AuxiliaryRecovery, RequiresExplicitLossAcceptance) {
  const auto inspected = auxiliary_token();
  EXPECT_EQ(dispatch({"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--expect-snapshot", inspected, "--output", output.string()}), 2);
  EXPECT_NE(stderr_stream.str().find("--accept-token-and-metadata-loss"), std::string::npos);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(AuxiliaryRecovery, RequiresSeparateStateConfirmation) {
  const auto inspected = auxiliary_token();
  EXPECT_EQ(dispatch({"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--expect-snapshot", inspected, "--accept-token-and-metadata-loss", "--output", output.string()}), 2);
  EXPECT_NE(stderr_stream.str().find("--confirm-separate-auxiliary-state"), std::string::npos);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(AuxiliaryRecovery, BothSnapshotsAreBoundToInspection) {
  const auto inspected = auxiliary_token();
  write(input.string() + ".bak", std::string(128, '\0'));
  EXPECT_EQ(reset(inspected), 2);
  EXPECT_FALSE(fs::exists(output));
  const auto fresh = auxiliary_token();
  write(input, std::string(64, '\0'));
  EXPECT_EQ(reset(fresh), 2);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(AuxiliaryRecovery, RejectsNonzeroMissingOrUninspectableCopies) {
  for (const auto &path : {input, fs::path(input.string() + ".bak")}) {
    for (const auto &bytes : {std::string("{\"root\":{\"session_tokens\":[]}}"), std::string("{broken"), std::string(" \n"), std::string("\0x", 2)}) {
      write(path, bytes);
      EXPECT_EQ(inspect(), 2);
      EXPECT_FALSE(fs::exists(output));
    }
    fs::remove(path);
    EXPECT_EQ(inspect(), 2);
    fs::create_directory(path);
    EXPECT_EQ(inspect(), 2);
    fs::remove(path);
    write(path, std::string(512, '\0'));
  }
}

TEST_F(AuxiliaryRecovery, DoesNotAcceptPairingOrCustomStateFilenames) {
  const auto auxiliary = input;
  for (const auto name : {"sunshine_state.json", "custom.json"}) {
    input = directory / name;
    write(input, std::string(20, '\0'));
    write(input.string() + ".bak", std::string(20, '\0'));
    EXPECT_EQ(inspect(), 2);
    EXPECT_FALSE(fs::exists(output));
  }
  input = auxiliary;
}

TEST_F(AuxiliaryRecovery, EmptySnapshotsAreExplicitlyResettableButOutputCannotBeReused) {
  write(input, "");
  write(input.string() + ".bak", "");
  const auto inspected = auxiliary_token();
  ASSERT_EQ(reset(inspected), 0) << stderr_stream.str();
  const auto candidate = read(output / "vibeshine_state.reset.json");
  EXPECT_EQ(reset(inspected), 2);
  EXPECT_EQ(read(output / "vibeshine_state.reset.json"), candidate);
}

TEST_F(AuxiliaryRecovery, RejectsUnknownRepeatedAndMissingOptions) {
  for (const auto &arguments : std::vector<std::vector<std::string>> {
         {"sunshine", "--auxiliary-recovery"},
         {"sunshine", "--auxiliary-recovery", "inspect", input.string(), "--accept-token-and-metadata-loss"},
         {"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--expect-snapshot"},
         {"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--accept-token-and-metadata-loss", "--accept-token-and-metadata-loss"},
         {"sunshine", "--auxiliary-recovery", "prepare", input.string()},
         {"sunshine", "--auxiliary-recovery", "prepare-reset", input.string(), "--unknown"}
       }) {
    EXPECT_EQ(dispatch(arguments), 2);
    EXPECT_FALSE(fs::exists(output));
  }
}

#ifndef _WIN32
TEST_F(AuxiliaryRecovery, RejectsSymlinksAndCreatesPrivateOutput) {
  const auto primary = read(input);
  fs::rename(input, directory / "saved-input");
  fs::create_symlink(directory / "saved-input", input);
  EXPECT_EQ(inspect(), 2);
  fs::remove(input);
  write(input, primary);
  ASSERT_EQ(reset(auxiliary_token()), 0);
  struct stat information {};
  ASSERT_EQ(::stat(output.c_str(), &information), 0);
  EXPECT_EQ(information.st_mode & 0777, 0700);
  for (const auto &entry : fs::directory_iterator(output)) {
    ASSERT_EQ(::stat(entry.path().c_str(), &information), 0);
    EXPECT_EQ(information.st_mode & 0777, 0600);
  }
}
#endif

TEST_F(AuxiliaryRecovery, OversizeZeroFilesCannotBypassInputBound) {
  write(input, std::string(16 * 1024 * 1024 + 1, '\0'));
  EXPECT_EQ(inspect(), 2);
  EXPECT_FALSE(fs::exists(output));
}

#ifndef _WIN32
TEST_F(AuxiliaryRecovery, PartialWriteCannotProduceCompleteResetManifest) {
  const auto inspected = auxiliary_token();
  const auto primary = read(input), backup = read(input.string() + ".bak");
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit {16, 16};
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      std::_Exit(3);
    }
    std::_Exit(reset(inspected) == 2 ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(read(input), primary);
  EXPECT_EQ(read(input.string() + ".bak"), backup);
  EXPECT_FALSE(fs::exists(output / "recovery.json"));
  EXPECT_FALSE(fs::exists(output / "vibeshine_state.reset.json"));
}
#endif

TEST_F(AuxiliaryRecovery, Utf8DispatcherPreservesUnicodeInputAndOutputPaths) {
  const auto unicode = directory / std::filesystem::path(u8"caf\u00e9 \u6d4b\u8bd5 \U0001f4c1");
  ASSERT_TRUE(fs::create_directory(unicode));
  input = unicode / "vibeshine_state.json";
  output = unicode / std::filesystem::path(u8"r\u00e9sultat \u8f93\u51fa");
  write(input, std::string(32, '\0'));
  auto backup = input;
  backup += ".bak";
  write(backup, std::string(16, '\0'));
  const auto encode = [](const fs::path &path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
  };
  // dispatch explicitly accepts UTF-8. The Windows native entry adapter is
  // separately exercised below; never supply an ACP .string() to this boundary.
  ASSERT_EQ(dispatch({"sunshine", "--auxiliary-recovery", "inspect", encode(input)}), 0) << stderr_stream.str();
  const std::string token = json::parse(stdout_stream.str()).at("snapshot_token");
  ASSERT_EQ(dispatch({"sunshine", "--auxiliary-recovery", "prepare-reset", encode(input), "--expect-snapshot", token, "--accept-token-and-metadata-loss", "--confirm-separate-auxiliary-state", "--output", encode(output)}), 0) << stderr_stream.str();
  EXPECT_TRUE(fs::exists(output / "recovery.json"));
  EXPECT_EQ(read(input), std::string(32, '\0'));
}

TEST_F(AuxiliaryRecovery, NativeBoundaryLeavesUnrelatedCliAlone) {
  const char *arguments[] {"sunshine", "--config", "non-UTF8-\xE9"};
  EXPECT_EQ(pairing_recovery::dispatch_native(3, arguments, stdout_stream, stderr_stream), std::nullopt);
  EXPECT_TRUE(stdout_stream.str().empty());
  EXPECT_TRUE(stderr_stream.str().empty());
}

#ifdef _WIN32
TEST_F(AuxiliaryRecovery, WindowsWideCommandLinePreservesCp1252AndNonAcpPaths) {
  for (const std::wstring name : {L"caf\u00e9 \u20ac", L"\u6d4b\u8bd5 \U0001f4c1"}) {
    const auto unicode = directory / name;
    ASSERT_TRUE(fs::create_directory(unicode));
    input = unicode / L"vibeshine_state.json";
    output = unicode / (L"output " + name);
    write(input, std::string(32, '\0'));
    auto backup = input;
    backup += L".bak";
    write(backup, std::string(16, '\0'));
    const auto prefix = L"\"C:\\Program Files\\Sunshine\\sunshine.exe\" --auxiliary-recovery ";
    const auto inspect_command = prefix + std::wstring(L"inspect \"") + input.wstring() + L"\"";
    stdout_stream.str({});
    stderr_stream.str({});
    ASSERT_EQ(pairing_recovery::dispatch_windows_command_line(inspect_command.c_str(), stdout_stream, stderr_stream), 0) << stderr_stream.str();
    const std::string token = json::parse(stdout_stream.str()).at("snapshot_token");
    const auto prepare_command = prefix + std::wstring(L"prepare-reset \"") + input.wstring() + L"\" --expect-snapshot " +
                                 std::wstring(token.begin(), token.end()) + L" --accept-token-and-metadata-loss --confirm-separate-auxiliary-state --output \"" + output.wstring() + L"\"";
    ASSERT_EQ(pairing_recovery::dispatch_windows_command_line(prepare_command.c_str(), stdout_stream, stderr_stream), 0) << stderr_stream.str();
    EXPECT_TRUE(fs::exists(output / "recovery.json"));
    EXPECT_EQ(read(input), std::string(32, '\0'));
    EXPECT_EQ(read(backup), std::string(16, '\0'));
  }
}

TEST_F(AuxiliaryRecovery, WindowsWideConversionRejectsInvalidUtf16WithoutOutput) {
  const auto command = std::wstring(L"sunshine --auxiliary-recovery inspect \"") + wchar_t(0xD800) + L"\"";
  EXPECT_EQ(pairing_recovery::dispatch_windows_command_line(command.c_str(), stdout_stream, stderr_stream), 2);
  EXPECT_NE(stderr_stream.str().find("invalid Unicode"), std::string::npos);
  EXPECT_FALSE(fs::exists(output));
}
#endif

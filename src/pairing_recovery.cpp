/**
 * @file src/pairing_recovery.cpp
 * @brief Offline, snapshot-bound preparation of duplicate-client recovery candidates.
 */
#include "pairing_recovery.h"

#include "crypto.h"
#include "paired_state_policy.h"
#include "state_storage_policy.h"

#include <boost/property_tree/json_parser.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
// clang-format off: SDDL declarations require the Windows types first.
  #include <Windows.h>
  #include <sddl.h>
  #include <shellapi.h>
// clang-format on
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace pairing_recovery {
  namespace {
    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;
    using json = nlohmann::json;
    constexpr std::size_t max_snapshot_bytes = 16 * 1024 * 1024;
    constexpr std::size_t max_clients = 256;

    struct recovery_error_t: std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    // Never put parser exceptions, certificates or arbitrary values in diagnostics.
    void require(bool condition, const char *message) {
      if (!condition) {
        throw recovery_error_t(message);
      }
    }

    fs::path utf8_path(std::string_view value) {
      return fs::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
    }

    std::string sha256(std::string_view bytes) {
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int length = 0;
      require(EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) == 1, "Could not hash the snapshot.");
      std::ostringstream result;
      for (unsigned int i = 0; i < length; ++i) {
        result << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(digest[i]);
      }
      return result.str();
    }

    std::optional<std::string> certificate_identity(const std::string &pem) {
      if (pem.empty() || pem.size() > 65536) {
        return std::nullopt;
      }
      std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
      if (!input) {
        return std::nullopt;
      }
      std::unique_ptr<X509, decltype(&X509_free)> certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
      if (!certificate) {
        return std::nullopt;
      }
      const auto length = i2d_X509(certificate.get(), nullptr);
      if (length <= 0 || length > 65536) {
        return std::nullopt;
      }
      std::string identity(static_cast<std::size_t>(length), '\0');
      auto *cursor = reinterpret_cast<unsigned char *>(identity.data());
      if (i2d_X509(certificate.get(), &cursor) != length) {
        return std::nullopt;
      }
      return identity;
    }

    std::string read_snapshot(const fs::path &path) {
      // Validate the opened object, not a separate pre-open path inspection.
      // No source is ever opened for writing, and special files cannot block us.
#ifdef _WIN32
      struct input_t {
        HANDLE file;

        ~input_t() {
          if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
          }
        }
      } input {CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};

      require(input.file != INVALID_HANDLE_VALUE, "Could not open the snapshot.");
      BY_HANDLE_FILE_INFORMATION information {};
      require(GetFileInformationByHandle(input.file, &information) && GetFileType(input.file) == FILE_TYPE_DISK && !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "Snapshot must be an existing regular file, not a link.");
#else
      struct input_t {
        int file;

        ~input_t() {
          if (file >= 0) {
            ::close(file);
          }
        }
      } input {::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};

      require(input.file >= 0, "Could not open the snapshot.");
      struct stat information {};
      require(::fstat(input.file, &information) == 0 && S_ISREG(information.st_mode), "Snapshot must be an existing regular file, not a link.");
#endif
      std::string bytes;
      char chunk[8192];
      for (;;) {
#ifdef _WIN32
        DWORD count = 0;
        require(ReadFile(input.file, chunk, sizeof(chunk), &count, nullptr) != 0, "Could not read the complete snapshot.");
#else
        const auto count = ::read(input.file, chunk, sizeof(chunk));
        require(count >= 0, "Could not read the complete snapshot.");
#endif
        if (!count) {
          break;
        }
        require(bytes.size() + static_cast<std::size_t>(count) <= max_snapshot_bytes, "Snapshot exceeds the 16 MiB recovery limit.");
        bytes.append(chunk, static_cast<std::size_t>(count));
      }
      return bytes;
    }

    json parse_snapshot(const std::string &bytes) {
      std::vector<std::set<std::string>> keys;
      try {
        return json::parse(bytes, [&keys](int depth, json::parse_event_t event, json &value) {
          require(depth <= 32, "Snapshot nesting exceeds the recovery limit.");
          if (event == json::parse_event_t::object_start) {
            keys.emplace_back();
          }
          if (event == json::parse_event_t::key) {
            require(keys.back().insert(value.get<std::string>()).second, "Duplicate JSON object keys require manual repair.");
          }
          if (event == json::parse_event_t::object_end) {
            keys.pop_back();
          }
          return true;
        });
      } catch (...) {
        throw recovery_error_t("Snapshot is not unambiguous, bounded JSON.");
      }
    }

    pt::ptree property_tree(const json &value) {
      pt::ptree result;
      std::istringstream input(value.dump());
      pt::read_json(input, result);
      return result;
    }

    struct span_t {
      std::size_t begin, end;
    };

    std::size_t skip_space(std::string_view bytes, std::size_t offset) {
      const auto next = bytes.find_first_not_of(" \t\r\n", offset);
      require(next != std::string_view::npos, "Unexpected end of the snapshot.");
      return next;
    }

    // JSON has already been syntax/depth/duplicate-key validated. Retain source
    // spans so unknown numeric values, scalar types and all other fields remain
    // byte-for-byte intact instead of round-tripping through floating point.
    span_t value_span(std::string_view bytes, std::size_t offset) {
      const auto begin = skip_space(bytes, offset);
      bool quoted = false;
      unsigned nesting = 0;
      for (auto i = begin; i < bytes.size(); ++i) {
        const auto ch = bytes[i];
        if (quoted) {
          if (ch == '\\') {
            ++i;
            continue;
          }
          if (ch == '"') {
            quoted = false;
            if (!nesting) {
              return {begin, i + 1};
            }
          }
        } else if (ch == '"') {
          quoted = true;
        } else if (ch == '[' || ch == '{') {
          ++nesting;
        } else if (ch == ']' || ch == '}') {
          if (!nesting) {
            return {begin, i};
          }
          if (!--nesting) {
            return {begin, i + 1};
          }
        } else if (!nesting && (ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
          return {begin, i};
        }
      }
      return {begin, bytes.size()};
    }

    span_t member_span(std::string_view bytes, span_t object, const std::string &name) {
      auto offset = object.begin + 1;
      while ((offset = skip_space(bytes, offset)) < object.end && bytes[offset] != '}') {
        const auto key = value_span(bytes, offset);
        const auto colon = skip_space(bytes, key.end);
        require(bytes[colon] == ':', "Invalid object boundary in the snapshot.");
        const auto value = value_span(bytes, colon + 1);
        if (json::parse(bytes.substr(key.begin, key.end - key.begin)).get<std::string>() == name) {
          return value;
        }
        offset = skip_space(bytes, value.end);
        if (bytes[offset] == ',') {
          ++offset;
        }
      }
      throw recovery_error_t("Missing named-client state boundary.");
    }

    std::vector<span_t> named_record_spans(std::string_view bytes) {
      const auto bom = bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u;
      const auto root = member_span(bytes, value_span(bytes, bom), "root");
      const auto named = member_span(bytes, root, "named_devices");
      std::vector<span_t> spans;
      auto offset = named.begin + 1;
      while ((offset = skip_space(bytes, offset)) < named.end && bytes[offset] != ']') {
        const auto value = value_span(bytes, offset);
        spans.push_back(value);
        offset = skip_space(bytes, value.end);
        if (bytes[offset] == ',') {
          ++offset;
        }
      }
      return spans;
    }

    struct analysis_t {
      json candidate;
      json groups = json::array();
      std::vector<std::vector<std::size_t>> duplicates;
      std::vector<span_t> source_spans;
      std::vector<std::size_t> source_indices;
      std::set<std::size_t> removed_indices;
      bool changed = false;
    };

    bool record_enabled(const json &record) {
      // Match the native loader: legacy disabled state wins, and only known
      // permission bits contribute authority. Keep the original record intact.
      if (!util::get_non_string_json_value<bool>(record, "enabled", true)) {
        return false;
      }
      const auto permissions = static_cast<crypto::PERM>(util::get_non_string_json_value<std::uint32_t>(
        record, "perm", static_cast<std::uint32_t>(crypto::PERM::_all))) & crypto::PERM::_all;
      return !!permissions;
    }

    analysis_t analyze(const std::string &bytes) {
      analysis_t result;
      result.candidate = parse_snapshot(bytes);
      require(result.candidate.is_object() && result.candidate.contains("root") && result.candidate["root"].is_object(), "Recovery requires an existing host identity and named-client state.");
      auto &root = result.candidate["root"];
      require(root.contains("named_devices") && root["named_devices"].is_array() && root["named_devices"].size() <= max_clients, "Recovery requires at most 256 named-client records.");
      auto &named = root["named_devices"];
      result.source_spans = named_record_spans(bytes);
      require(result.source_spans.size() == named.size(), "Could not identify named-client state boundaries.");

      // The only lossless automatic case: an entire record repeated under the
      // same UUID. Different UUIDs can own settings elsewhere and need selection.
      json unique = json::array();
      std::map<std::string, std::size_t> uuids;
      for (std::size_t index = 0; index < named.size(); ++index) {
        const auto &record = named[index];
        require(record.is_object() && record.contains("uuid") && record["uuid"].is_string(), "Invalid named-client UUID field.");
        const auto uuid = record["uuid"].get<std::string>();
        const auto [previous, inserted] = uuids.emplace(uuid, unique.size());
        if (!inserted) {
          const auto first = result.source_spans[result.source_indices[previous->second]];
          const auto repeated = result.source_spans[index];
          require(bytes.substr(first.begin, first.end - first.begin) == bytes.substr(repeated.begin, repeated.end - repeated.begin), "Conflicting or nonidentical records share a UUID; automatic recovery is unavailable.");
          result.removed_indices.insert(index);
          result.changed = true;
        } else {
          result.source_indices.push_back(index);
          unique.push_back(record);
        }
      }
      named = std::move(unique);
      require(statefile::policy::valid_primary_state(property_tree(result.candidate), false), "Snapshot has invalid host identity, credentials or client structure.");
      auto typed = result.candidate;
      // Validate every native field, even on records that may be removed. Only
      // repeated certificates are permitted during this inspection phase.
      require(nvhttp::state_policy::normalize_snapshot(typed, certificate_identity, true), "Snapshot has invalid native client fields.");
      std::map<std::string, std::vector<std::size_t>> identities;
      for (std::size_t i = 0; i < named.size(); ++i) {
        const auto identity = certificate_identity(named[i]["cert"].get<std::string>());
        require(identity.has_value(), "A named-client certificate is invalid.");
        identities[*identity].push_back(i);
      }
      for (const auto &[identity, indices] : identities) {
        if (indices.size() < 2) {
          continue;
        }
        json group = {{"certificate_sha256", sha256(identity)}, {"records", json::array()}};
        for (const auto index : indices) {
          const auto &record = named[index];
          const auto enabled = record_enabled(record);
          group["records"].push_back({{"index", result.source_indices[index]}, {"uuid", record["uuid"]}, {"name", record["name"]}, {"enabled", enabled}});
        }
        result.groups.push_back(std::move(group));
        result.duplicates.push_back(indices);
      }
      return result;
    }

    void reconcile(analysis_t &analysis, const std::vector<std::string> &keep) {
      std::set<std::string> choices(keep.begin(), keep.end());
      require(choices.size() == keep.size(), "A survivor UUID was selected more than once.");
      auto &named = analysis.candidate["root"]["named_devices"];
      std::set<std::size_t> remove;
      for (const auto &indices : analysis.duplicates) {
        std::optional<std::size_t> selected;
        bool revoked = false;
        for (const auto index : indices) {
          const auto &record = named[index];
          revoked = revoked || !record_enabled(record);
          if (choices.erase(record["uuid"].get<std::string>())) {
            require(!selected, "Select exactly one existing UUID per duplicate-certificate group.");
            selected = index;
          }
        }
        require(selected.has_value(), "Select one existing survivor UUID for every duplicate-certificate group.");
        require(!revoked || !record_enabled(named[*selected]), "An existing revocation forbids an enabled survivor. Select a disabled record; reauthorize later through normal pairing.");
        for (const auto index : indices) {
          if (index != *selected) {
            remove.insert(index);
            analysis.removed_indices.insert(analysis.source_indices[index]);
          }
        }
      }
      require(choices.empty(), "A selected UUID does not belong to a duplicate-certificate group.");
      for (auto it = remove.rbegin(); it != remove.rend(); ++it) {
        named.erase(named.begin() + static_cast<json::difference_type>(*it));
      }
      analysis.changed = analysis.changed || !remove.empty();
      auto validated = property_tree(analysis.candidate);
      require(statefile::policy::normalize_legacy_pairing_state(validated, certificate_identity) && statefile::policy::valid_primary_state(validated, false), "Candidate still fails durable pairing-state validation; no recovery files were created.");
      auto typed = analysis.candidate;
      require(nvhttp::state_policy::normalize_snapshot(typed, certificate_identity), "Candidate still fails native pairing-state validation; no recovery files were created.");
      require(analysis.changed, "No duplicate named-client records need reconciliation.");
    }

    std::string candidate_bytes(const std::string &bytes, const analysis_t &analysis) {
      std::vector<span_t> removals;
      for (auto it = analysis.removed_indices.begin(); it != analysis.removed_indices.end();) {
        const auto first = *it;
        auto last = first;
        while (++it != analysis.removed_indices.end() && *it == last + 1) {
          last = *it;
        }
        if (last + 1 < analysis.source_spans.size()) {
          removals.push_back({analysis.source_spans[first].begin, analysis.source_spans[last + 1].begin});
        } else {
          require(first > 0, "Recovery must retain a named client.");
          removals.push_back({analysis.source_spans[first - 1].end, analysis.source_spans[last].end});
        }
      }
      auto result = bytes;
      for (auto it = removals.rbegin(); it != removals.rend(); ++it) {
        result.erase(it->begin, it->end - it->begin);
      }
      require(parse_snapshot(result) == analysis.candidate, "Prepared state does not match the reviewed reconciliation.");
      return result;
    }

#ifdef _WIN32
    struct private_security_t {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

      private_security_t() {
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr) != 0, "Could not create the private recovery security descriptor.");
        attributes.lpSecurityDescriptor = descriptor;
      }

      ~private_security_t() {
        LocalFree(descriptor);
      }
    };
#endif

    void create_private_directory(const fs::path &path) {
#ifdef _WIN32
      // New output only; never rewrite an existing/source object's ACL.
      private_security_t security;
      require(CreateDirectoryW(path.c_str(), &security.attributes) != 0, "Output must be a new private directory in an existing parent.");
#else
      require(::mkdir(path.c_str(), 0700) == 0, "Output must be a new private directory in an existing parent.");
#endif
    }

    void write_new_file(const fs::path &path, std::string_view bytes) {
#ifdef _WIN32
      // Apply an explicit private ACL to every file too: do not trust a parent
      // directory's inherited ACL if its pathname was replaced between opens.
      private_security_t security;
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      require(file != INVALID_HANDLE_VALUE, "Could not create a new recovery file.");
      DWORD written = 0;
      const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
      CloseHandle(file);
      require(complete, "Could not persist recovery files; the incomplete output directory must not be used.");
#else
      const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      require(file >= 0, "Could not create a new recovery file.");
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
          break;
        }
        offset += static_cast<std::size_t>(count);
      }
      const bool complete = offset == bytes.size() && ::fsync(file) == 0;
      const int close_result = ::close(file);
      require(complete && close_result == 0, "Could not persist recovery files; the incomplete output directory must not be used.");
#endif
    }
  }  // namespace

  int run(int argc, const char *const argv[], std::ostream &out, std::ostream &error) {
    try {
      require(argc >= 2, "Usage: --pairing-recovery inspect INPUT | prepare INPUT --expect-snapshot TOKEN --keep UUID [...] --output NEW_DIRECTORY");
      const std::string_view operation = argv[0];
      require(operation == "inspect" || operation == "prepare", "Recovery operation must be inspect or prepare.");
      std::string expected, destination;
      std::vector<std::string> keep;
      for (int i = 2; i < argc; i += 2) {
        require(operation == "prepare" && i + 1 < argc, "Invalid recovery options.");
        const std::string_view option = argv[i];
        if (option == "--keep") {
          require(keep.size() < max_clients, "Too many survivor selections.");
          keep.emplace_back(argv[i + 1]);
        } else if (option == "--expect-snapshot" && expected.empty()) {
          expected = argv[i + 1];
        } else if (option == "--output" && destination.empty()) {
          destination = argv[i + 1];
        } else {
          throw recovery_error_t("Unknown or repeated recovery option.");
        }
      }
      const auto source = utf8_path(argv[1]);
      const auto bytes = read_snapshot(source);
      auto backup_path = source;
      backup_path += ".bak";
      std::optional<std::string> backup;
      if (fs::symlink_status(backup_path).type() != fs::file_type::not_found) {
        backup = read_snapshot(backup_path);
      }
      const auto primary_hash = sha256(bytes);
      const auto backup_hash = backup ? sha256(*backup) : "missing";
      const auto token = sha256(primary_hash + ":" + backup_hash);
      auto analysis = analyze(bytes);
      json report = {{"schema_version", 1}, {"snapshot_token", token}, {"primary_sha256", primary_hash}, {"backup_sha256", backup_hash}, {"duplicate_groups", analysis.groups}, {"identical_uuid_duplicates_removed", analysis.changed}};
      if (operation == "inspect") {
        out << report.dump(2, ' ', true) << '\n';
        return 0;
      }
      require(!expected.empty() && expected == token, "Snapshot changed or --expect-snapshot does not match inspection. Inspect the current copies again.");
      require(!destination.empty(), "Preparation requires --output NEW_DIRECTORY.");
      reconcile(analysis, keep);
      const auto candidate = candidate_bytes(bytes, analysis);
      require(candidate.size() <= max_snapshot_bytes, "Serialized candidate exceeds the state-file size limit.");
      report["candidate_sha256"] = sha256(candidate);
      report["selected_uuids"] = keep;
      report["complete"] = true;
      const auto output = utf8_path(destination);
      create_private_directory(output);
      write_new_file(output / "primary.original.json", bytes);
      if (backup) {
        write_new_file(output / "backup.original.json", *backup);
      }
      write_new_file(output / "sunshine_state.reconciled.json", candidate);
      // Published last. No complete manifest means the directory is incomplete.
      write_new_file(output / "recovery.json", report.dump(2, ' ', true) + "\n");
      out << "Recovery candidate prepared. Original snapshots were not changed. Review recovery.json before any offline installation.\n";
      return 0;
    } catch (const recovery_error_t &exception) {
      error << exception.what() << '\n';
      return 2;
    } catch (const std::exception &) {
      // Library exceptions can contain certificates or arbitrary input values.
      error << "Recovery failed while reading, validating or writing snapshots; originals were not changed.\n";
      return 2;
    }
  }

  // Reuse the bounded, no-follow input and private, create-only output helpers.
  // This is an explicit destructive reset candidate, never automatic recovery.
  int run_auxiliary(int argc, const char *const argv[], std::ostream &out, std::ostream &error) {
    try {
      require(argc >= 2, "Usage: --auxiliary-recovery inspect INPUT | prepare-reset INPUT --expect-snapshot TOKEN --accept-token-and-metadata-loss --confirm-separate-auxiliary-state --output NEW_DIRECTORY");
      const std::string_view operation = argv[0];
      require(operation == "inspect" || operation == "prepare-reset", "Auxiliary operation must be inspect or prepare-reset.");
      std::string expected, destination;
      bool accept_loss = false;
      bool confirm_separate = false;
      for (int i = 2; i < argc; ++i) {
        require(operation == "prepare-reset", "Inspection does not accept reset options.");
        const std::string_view option = argv[i];
        if (option == "--accept-token-and-metadata-loss" && !accept_loss) {
          accept_loss = true;
        } else if (option == "--confirm-separate-auxiliary-state" && !confirm_separate) {
          confirm_separate = true;
        } else if (option == "--expect-snapshot" && expected.empty() && i + 1 < argc) {
          expected = argv[++i];
        } else if (option == "--output" && destination.empty() && i + 1 < argc) {
          destination = argv[++i];
        } else {
          throw recovery_error_t("Unknown, repeated or incomplete auxiliary recovery option.");
        }
      }
      const auto source = utf8_path(argv[1]);
      // Zero-filled snapshots have no embedded identity. The name check catches
      // obvious mistakes; the operator must confirm configured path separation.
      require(source.filename() == "vibeshine_state.json", "Use copies of the configured vibeshine_state.json and its .bak. Custom filenames are not supported.");
      const auto bytes = read_snapshot(source);
      auto backup_path = source;
      backup_path += ".bak";
      const auto backup = read_snapshot(backup_path);
      const auto zero_filled = [](const std::string &contents) {
        return contents.find_first_not_of('\0') == std::string::npos;
      };
      require(zero_filled(bytes) && zero_filled(backup), "Reset is limited to two existing empty or entirely zero-filled auxiliary snapshots. Nonzero, missing or unreadable state requires separate investigation; no files were changed.");
      const auto primary_hash = sha256(bytes);
      const auto backup_hash = sha256(backup);
      const auto token = sha256("auxiliary-reset:" + primary_hash + ":" + backup_hash);
      json report = {{"schema_version", 1}, {"operation", "auxiliary_reset_not_recovery"}, {"snapshot_token", token}, {"primary_sha256", primary_hash}, {"backup_sha256", backup_hash}, {"lost_state", {"API tokens", "Web UI sessions", "application ID aliases", "display metadata", "crash dismissal and notification metadata"}}, {"warning", "This cannot recover lost data. Verify the configured auxiliary file is separate from pairing state and Web UI credentials before preparing or installing a reset."}};
      if (operation == "inspect") {
        out << report.dump(2, ' ', true) << '\n';
        return 0;
      }
      require(accept_loss, "Reset requires --accept-token-and-metadata-loss. Existing API tokens, Web UI sessions and auxiliary metadata cannot be recovered.");
      require(confirm_separate, "Reset requires --confirm-separate-auxiliary-state after verifying the configured auxiliary and pairing paths differ.");
      require(!expected.empty() && expected == token, "Snapshot changed or --expect-snapshot does not match inspection. Inspect the current copies again.");
      require(!destination.empty(), "Preparation requires --output NEW_DIRECTORY.");
      // Explicit empty token containers also prevent startup migration from
      // resurrecting stale tokens retained in older primary-state snapshots.
      const std::string candidate = "{\n  \"root\": {\n    \"api_tokens\": [],\n    \"session_tokens\": []\n  }\n}\n";
      const auto output = utf8_path(destination);
      create_private_directory(output);
      write_new_file(output / "primary.original.json", bytes);
      write_new_file(output / "backup.original.json", backup);
      write_new_file(output / "vibeshine_state.reset.json", candidate);
      write_new_file(output / "vibeshine_state.reset.json.bak", candidate);
      require(read_snapshot(output / "primary.original.json") == bytes && read_snapshot(output / "backup.original.json") == backup && read_snapshot(output / "vibeshine_state.reset.json") == candidate && read_snapshot(output / "vibeshine_state.reset.json.bak") == candidate, "Output read-back verification failed; do not use the incomplete output directory.");
      report["candidate_sha256"] = sha256(candidate);
      report["operator_confirmed_separate_auxiliary_state"] = true;
      report["complete"] = true;
      // Complete manifest is published last, as for pairing recovery.
      write_new_file(output / "recovery.json", report.dump(2, ' ', true) + "\n");
      out << "Auxiliary RESET candidate prepared, not installed. No lost data was recovered. Original snapshots, pairing state and credentials were not changed. Review recovery.json before offline installation of both reset copies.\n";
      return 0;
    } catch (const recovery_error_t &exception) {
      error << exception.what() << '\n';
      return 2;
    } catch (const std::exception &) {
      error << "Auxiliary reset preparation failed; originals were not changed. Do not use an output directory without a complete recovery.json manifest.\n";
      return 2;
    }
  }

  std::optional<int> dispatch(int argc, const char *const argv[], std::ostream &out, std::ostream &error) {
    if (argc >= 2 && std::string_view(argv[1]) == "--auxiliary-recovery") {
      return run_auxiliary(argc - 2, argv + 2, out, error);
    }
    if (argc < 2 || std::string_view(argv[1]) != "--pairing-recovery") {
      return std::nullopt;
    }
    return run(argc - 2, argv + 2, out, error);
  }
#ifdef _WIN32
  std::optional<int> dispatch_windows_command_line(const wchar_t *command_line, std::ostream &out, std::ostream &error) {
    try {
      require(command_line && *command_line, "Could not read the wide recovery command line.");
      int count = 0;
      auto *wide = CommandLineToArgvW(command_line, &count);
      require(wide != nullptr, "Could not parse the wide recovery command line.");
      const auto release = [](wchar_t **arguments) {
        LocalFree(arguments);
      };
      std::unique_ptr<wchar_t *, decltype(release)> owner(wide, release);
      if (count < 2 || (std::wstring_view(wide[1]) != L"--pairing-recovery" && std::wstring_view(wide[1]) != L"--auxiliary-recovery")) {
        return std::nullopt;
      }
      std::vector<std::string> utf8;
      utf8.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0, nullptr, nullptr);
        require(size > 0, "Recovery arguments contain invalid Unicode.");
        std::string converted(static_cast<std::size_t>(size), '\0');
        require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, converted.data(), size, nullptr, nullptr) == size, "Could not convert recovery arguments to UTF-8.");
        converted.pop_back();  // The conversion count includes the terminating NUL.
        utf8.push_back(std::move(converted));
      }
      std::vector<const char *> arguments;
      arguments.reserve(utf8.size());
      for (const auto &argument : utf8) {
        arguments.push_back(argument.c_str());
      }
      return dispatch(count, arguments.data(), out, error);
    } catch (const recovery_error_t &exception) {
      error << exception.what() << '\n';
    } catch (const std::exception &) {
      error << "Could not prepare Unicode recovery arguments; no snapshots were changed.\n";
    }
    return 2;
  }
#endif

  std::optional<int> dispatch_native(int argc, const char *const argv[], std::ostream &out, std::ostream &error) {
#ifdef _WIN32
    // Only these ASCII flags are read from CRT argv. All actual arguments come
    // from UTF-16: ACP argv may already have replaced non-ACP characters with '?'.
    // Do not reinterpret or change encoding for unrelated host CLI options.
    if (argc < 2 || (std::string_view(argv[1]) != "--pairing-recovery" && std::string_view(argv[1]) != "--auxiliary-recovery")) {
      return std::nullopt;
    }
    const auto result = dispatch_windows_command_line(GetCommandLineW(), out, error);
    if (!result) {
      error << "Wide recovery command line did not match the explicit recovery flag.\n";
      return 2;
    }
    return result;
#else
    return dispatch(argc, argv, out, error);
#endif
  }
}  // namespace pairing_recovery

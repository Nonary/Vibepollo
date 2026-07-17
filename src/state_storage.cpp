#include "state_storage.h"

#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "utility.h"

#include <algorithm>
#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
  #include <winsock2.h>
  #include <Windows.h>
  #include <AclAPI.h>
#endif

using namespace std::literals;

namespace statefile {
  namespace {
    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;

    std::once_flag migration_once;

    enum class json_load_result_e {
      loaded,   ///< File existed and parsed cleanly.
      missing,  ///< File (or its content) was absent/blank; safe to create fresh.
      corrupt,  ///< File was readable but its content was not valid JSON; safe to discard and recreate.
      failed,   ///< File could not be inspected/opened (I/O, permission, or not-a-regular-file); do NOT overwrite.
    };

    /**
     * @brief Best-effort rename of an unparseable state file out of the way so a
     *        fresh, valid file can replace it. Preserves the bad copy for forensics.
     *
     * Callers hold state_mutex() during their read/modify/write transaction, so this
     * runs serialized with respect to other writers of the same file.
     */
    void quarantine_corrupt_state(const fs::path &path) {
      static std::atomic<unsigned> sequence {0};
      std::error_code ec;
      const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()
      )
                           .count();
      fs::path quarantine = path;
      // Include a process-unique counter so two corrupt-detections in the same
      // wall-clock second cannot overwrite each other's forensic copy.
      quarantine += ".corrupt-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1));
      fs::rename(path, quarantine, ec);
      if (ec) {
        // The subsequent atomic write replaces the file in place anyway, so this is non-fatal.
        BOOST_LOG(warning) << "statefile: could not quarantine corrupt file "sv << path.string()
                           << " (will overwrite in place): "sv << ec.message();
      } else {
        BOOST_LOG(warning) << "statefile: quarantined corrupt state file to "sv << quarantine.string();
      }
    }

    pt::ptree &ensure_root(pt::ptree &tree) {
      auto it = tree.find("root");
      if (it == tree.not_found()) {
        auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
        return inserted->second;
      }
      return it->second;
    }

    json_load_result_e load_tree_for_update(const fs::path &path, pt::ptree &out) {
      out = {};
      if (path.empty()) {
        BOOST_LOG(error) << "statefile: refusing to update empty state path";
        return json_load_result_e::failed;
      }

      std::error_code ec;
      if (!fs::exists(path, ec)) {
        if (ec) {
          BOOST_LOG(error) << "statefile: unable to inspect "sv << path.string() << ": "sv << ec.message();
          return json_load_result_e::failed;
        }
        return json_load_result_e::missing;
      }

      if (!fs::is_regular_file(path, ec) || ec) {
        if (ec) {
          BOOST_LOG(error) << "statefile: unable to inspect "sv << path.string() << ": "sv << ec.message();
        } else {
          BOOST_LOG(error) << "statefile: refusing to update non-file state path "sv << path.string();
        }
        return json_load_result_e::failed;
      }

      // Read the raw bytes ourselves so we can tell an I/O/permission failure
      // (which must NOT clobber the on-disk state) apart from genuinely malformed
      // content (which is safe to discard and recreate). boost::read_json folds
      // both cases into a single json_parser_error, so it cannot disambiguate them.
      // The read is done by size and validated against gcount so that a truncated
      // or failed read is reported as 'failed' (refuse), never mistaken for corrupt.
      std::string content;
      {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
          BOOST_LOG(error) << "statefile: refusing to update "sv << path.string()
                           << " - could not open it for reading"sv;
          return json_load_result_e::failed;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) {
          BOOST_LOG(error) << "statefile: refusing to update "sv << path.string()
                           << " - could not determine file size"sv;
          return json_load_result_e::failed;
        }
        if (size > 0) {
          in.seekg(0, std::ios::beg);
          content.resize(static_cast<size_t>(size));
          in.read(content.data(), size);
          if (in.bad() || in.gcount() != size) {
            BOOST_LOG(error) << "statefile: refusing to update "sv << path.string()
                             << " - read error or short read"sv;
            return json_load_result_e::failed;
          }
        }
      }

      // Strip a leading UTF-8 BOM so a BOM-prefixed but otherwise valid file is
      // parsed (and preserved) rather than discarded as corrupt.
      if (content.size() >= 3 &&
          static_cast<unsigned char>(content[0]) == 0xEF &&
          static_cast<unsigned char>(content[1]) == 0xBB &&
          static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
      }

      // A blank/whitespace-only file is a benign partial-write artifact; treat it
      // like a missing file so the caller recreates valid content.
      if (content.find_first_not_of(" \t\r\n\f\v"sv) == std::string::npos) {
        return json_load_result_e::missing;
      }

      try {
        pt::ptree parsed;
        std::istringstream is(content);
        pt::read_json(is, parsed);
        out = std::move(parsed);
        return json_load_result_e::loaded;
      } catch (const std::exception &e) {
        // Readable but not valid JSON: discard it so the user is not permanently
        // wedged (e.g. an un-dismissable crash banner) by a single bad file.
        BOOST_LOG(error) << "statefile: "sv << path.string()
                         << " contains malformed JSON ("sv << e.what()
                         << "); quarantining it and recreating from current state"sv;
        quarantine_corrupt_state(path);
        return json_load_result_e::corrupt;
      }
    }

    bool load_tree_if_exists(const fs::path &path, pt::ptree &out) {
      if (!fs::exists(path)) {
        return false;
      }
      try {
        pt::read_json(path.string(), out);
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "statefile: failed to read "sv << path.string() << ": "sv << e.what();
        return false;
      }
    }

    void write_tree(const fs::path &path, const pt::ptree &tree) {
      write_json_atomic(path.string(), tree);
    }

    bool parse_json_file(const fs::path &path, pt::ptree *tree = nullptr) {
      try {
        pt::ptree parsed;
        pt::read_json(path.string(), parsed);
        if (tree) {
          *tree = std::move(parsed);
        }
        return true;
      } catch (...) {
        return false;
      }
    }

    bool parse_json_string(const std::string &contents) {
      try {
        std::istringstream in(contents);
        pt::ptree parsed;
        pt::read_json(in, parsed);
        return true;
      } catch (...) {
        return false;
      }
    }

#ifdef _WIN32
    bool path_exists_or_may_be_access_denied(const fs::path &path) {
      if (path.empty()) {
        return false;
      }

      const auto wide_path = path.wstring();
      const DWORD attributes = GetFileAttributesW(wide_path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES) {
        return true;
      }

      switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
          return false;
        default:
          // Access-denied and sharing failures are exactly the cases this
          // repair path is meant to address. Try the ACL API anyway.
          return true;
      }
    }

    void clear_readonly_attribute(const fs::path &path) {
      if (path.empty()) {
        return;
      }
      const auto wide_path = path.wstring();
      const DWORD attributes = GetFileAttributesW(wide_path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_READONLY)) {
        return;
      }
      if (SetFileAttributesW(wide_path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY))) {
        BOOST_LOG(debug) << "statefile: cleared read-only attribute on "sv << path.string();
      } else {
        BOOST_LOG(warning) << "statefile: failed to clear read-only attribute on "sv << path.string()
                           << " (error="sv << GetLastError() << ')';
      }
    }

    bool set_token_privilege(LPCWSTR privilege_name, bool enable) {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw_token)) {
        return false;
      }
      auto close_token = util::fail_guard([&raw_token]() {
        if (raw_token) {
          CloseHandle(raw_token);
        }
      });

      LUID luid;
      if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) {
        return false;
      }
      TOKEN_PRIVILEGES privileges {};
      privileges.PrivilegeCount = 1;
      privileges.Privileges[0].Luid = luid;
      privileges.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
      if (!AdjustTokenPrivileges(raw_token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        return false;
      }
      return GetLastError() == ERROR_SUCCESS;
    }

    // Last-resort recovery: a botched upgrade can leave a config/state file owned by
    // a foreign account with a DACL the service cannot rewrite. The streaming host
    // runs as LocalSystem, so granting it ownership lets the inheritance reset retry
    // succeed. Best-effort and scoped to our own config paths. The required privileges
    // are enabled only for the duration of the ownership change, not process-wide.
    bool take_ownership_as_system(const fs::path &path) {
      const bool restore_enabled = set_token_privilege(L"SeRestorePrivilege", true);
      const bool take_enabled = set_token_privilege(L"SeTakeOwnershipPrivilege", true);
      auto disable_privileges = util::fail_guard([&]() {
        if (restore_enabled) {
          set_token_privilege(L"SeRestorePrivilege", false);
        }
        if (take_enabled) {
          set_token_privilege(L"SeTakeOwnershipPrivilege", false);
        }
      });
      if (!restore_enabled && !take_enabled) {
        return false;
      }

      alignas(DWORD) BYTE sid_buffer[SECURITY_MAX_SID_SIZE];
      DWORD sid_size = sizeof(sid_buffer);
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sid_buffer, &sid_size)) {
        return false;
      }

      const auto wide_path = path.wstring();
      const DWORD status = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(wide_path.c_str()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        sid_buffer,
        nullptr,
        nullptr,
        nullptr
      );
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "statefile: failed to take ownership of "sv << path.string()
                           << " (error="sv << status << ')';
        return false;
      }
      BOOST_LOG(info) << "statefile: took ownership of "sv << path.string() << " to repair permissions"sv;
      return true;
    }

    bool enable_acl_inheritance_once(const fs::path &path) {
      const auto wide_path = path.wstring();
      PSECURITY_DESCRIPTOR security_descriptor = nullptr;
      PACL current_dacl = nullptr;
      DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(wide_path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &current_dacl,
        nullptr,
        &security_descriptor
      );
      auto free_security_descriptor = util::fail_guard([&security_descriptor]() {
        if (security_descriptor) {
          LocalFree(security_descriptor);
        }
      });

      if (status == ERROR_SUCCESS && current_dacl) {
        status = SetNamedSecurityInfoW(
          const_cast<LPWSTR>(wide_path.c_str()),
          SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
          nullptr,
          nullptr,
          current_dacl,
          nullptr
        );
        if (status == ERROR_SUCCESS) {
          BOOST_LOG(debug) << "statefile: restored inherited ACLs for "sv << path.string();
          return true;
        }

        BOOST_LOG(warning) << "statefile: failed to restore inherited ACLs for "sv << path.string()
                           << " (error="sv << status << ')';
        return false;
      }
      if (status == ERROR_SUCCESS) {
        status = ERROR_INVALID_ACL;
      }

      // If the current DACL cannot be read, do not pass a null DACL together
      // with DACL_SECURITY_INFORMATION. Only try to clear the protected-DACL
      // bit; this avoids accidentally granting Everyone full access.
      const DWORD unprotect_status = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(wide_path.c_str()),
        SE_FILE_OBJECT,
        UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr
      );
      if (unprotect_status == ERROR_SUCCESS) {
        BOOST_LOG(debug) << "statefile: restored inherited ACLs for "sv << path.string();
        return true;
      }

      BOOST_LOG(warning) << "statefile: failed to inspect/restore inherited ACLs for "sv << path.string()
                         << " (get_error="sv << status << ", set_error="sv << unprotect_status << ')';
      return false;
    }

    bool enable_acl_inheritance(const fs::path &path) {
      if (!path_exists_or_may_be_access_denied(path)) {
        return false;
      }

      // A read-only attribute blocks atomic replaces independently of the DACL, so
      // clear it whenever we touch a config path during repair.
      clear_readonly_attribute(path);

      if (enable_acl_inheritance_once(path)) {
        return true;
      }

      // The DACL could not be read or rewritten (likely foreign ownership). Take
      // ownership as SYSTEM and retry once before giving up.
      if (take_ownership_as_system(path) && enable_acl_inheritance_once(path)) {
        BOOST_LOG(info) << "statefile: repaired ACLs for "sv << path.string() << " after taking ownership"sv;
        return true;
      }
      return false;
    }

    std::wstring normalized_path_key(fs::path path) {
      path = fs::absolute(path).lexically_normal();
      path.make_preferred();

      auto key = path.wstring();
      std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      return key;
    }

    bool path_is_at_or_under(const fs::path &candidate, const fs::path &root) {
      if (candidate.empty() || root.empty()) {
        return false;
      }

      try {
        auto candidate_key = normalized_path_key(candidate);
        auto root_key = normalized_path_key(root);
        while (root_key.size() > 3 && (root_key.back() == L'\\' || root_key.back() == L'/')) {
          root_key.pop_back();
        }

        if (candidate_key == root_key) {
          return true;
        }

        if (candidate_key.size() <= root_key.size() || candidate_key.compare(0, root_key.size(), root_key) != 0) {
          return false;
        }

        const wchar_t separator = candidate_key[root_key.size()];
        return separator == L'\\' || separator == L'/';
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "statefile: failed to normalize ACL repair path "
                           << candidate.string() << " under " << root.string()
                           << ": "sv << e.what();
        return false;
      }
    }

    void add_root_from_file(std::set<fs::path> &roots, const std::string &path) {
      if (path.empty()) {
        return;
      }

      fs::path candidate {path};
      const auto parent = candidate.parent_path();
      if (!parent.empty()) {
        roots.insert(parent);
      }
    }

    bool is_in_any_root(const fs::path &path, const std::set<fs::path> &roots) {
      return std::any_of(roots.begin(), roots.end(), [&](const fs::path &root) {
        return path_is_at_or_under(path, root);
      });
    }

    void add_file_if_in_root(std::set<fs::path> &files, const std::set<fs::path> &roots, const std::string &path) {
      if (path.empty()) {
        return;
      }

      fs::path candidate {path};
      if (is_in_any_root(candidate, roots)) {
        files.insert(candidate);
      }
    }
#endif
  }  // namespace

  std::mutex &state_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  void write_json_atomic(const std::string &path, const pt::ptree &tree) {
    if (path.empty()) {
      throw std::runtime_error("atomic JSON write path is empty");
    }

    std::ostringstream out;
    pt::write_json(out, tree);
    const std::string contents = out.str();
    if (!parse_json_string(contents)) {
      throw std::runtime_error("refusing to write malformed JSON");
    }

    const fs::path target(path);
    if (file_handler::write_file(path.c_str(), contents) != 0) {
      throw std::runtime_error("atomic JSON write failed");
    }

    if (!parse_json_file(target)) {
      throw std::runtime_error("atomic JSON write verification failed");
    }
  }

  bool load_json_for_update(const std::string &path, pt::ptree &tree) {
    return load_tree_for_update(fs::path(path), tree) != json_load_result_e::failed;
  }

  const std::string &sunshine_state_path() {
    return config::nvhttp.file_state;
  }

  const std::string &vibeshine_state_path() {
    if (!config::nvhttp.vibeshine_file_state.empty()) {
      return config::nvhttp.vibeshine_file_state;
    }
    return config::nvhttp.file_state;
  }

  bool secure_private_directory(const std::string &path) {
#ifdef _WIN32
    if (path.empty()) {
      return false;
    }
    fs::path dir(path);
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
      return false;
    }
    const auto wide_path = dir.wstring();
    const DWORD attributes = GetFileAttributesW(wide_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return false;
    }

    clear_readonly_attribute(dir);

    // Build an explicit, NON-inherited DACL granting full control to LocalSystem and
    // the local Administrators group only. Applied with PROTECTED_DACL so no
    // %ProgramFiles% "Users:(RX)" ACE can cascade in and expose the TLS private key.
    alignas(DWORD) BYTE system_sid[SECURITY_MAX_SID_SIZE];
    alignas(DWORD) BYTE admins_sid[SECURITY_MAX_SID_SIZE];
    DWORD system_size = sizeof(system_sid);
    DWORD admins_size = sizeof(admins_sid);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins_sid, &admins_size)) {
      return false;
    }

    // Always include the current process user so the running host never loses access
    // to its own credentials (e.g. sunshine.exe run directly as a non-admin user). When
    // running as the LocalSystem service this is the SYSTEM SID (already granted), so no
    // additional non-admin principal is added.
    alignas(TOKEN_USER) BYTE token_user_buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER) + 16];
    PSID user_sid = nullptr;
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
      return false;
    }
    DWORD token_len = 0;
    const bool token_user_ok = GetTokenInformation(
      process_token,
      TokenUser,
      token_user_buf,
      sizeof(token_user_buf),
      &token_len
    );
    CloseHandle(process_token);
    if (!token_user_ok) {
      return false;
    }
    PSID candidate = reinterpret_cast<TOKEN_USER *>(token_user_buf)->User.Sid;
    if (!candidate || !IsValidSid(candidate)) {
      return false;
    }
    if (!EqualSid(candidate, system_sid)) {
      user_sid = candidate;
    }

    EXPLICIT_ACCESS_W entries[3] {};
    ULONG entry_count = 0;
    const auto add_full_control = [&](PSID sid, TRUSTEE_TYPE trustee_type) {
      entries[entry_count].grfAccessPermissions = FILE_ALL_ACCESS;
      entries[entry_count].grfAccessMode = SET_ACCESS;
      entries[entry_count].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
      entries[entry_count].Trustee.TrusteeForm = TRUSTEE_IS_SID;
      entries[entry_count].Trustee.TrusteeType = trustee_type;
      entries[entry_count].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
      ++entry_count;
    };
    add_full_control(system_sid, TRUSTEE_IS_USER);
    add_full_control(admins_sid, TRUSTEE_IS_GROUP);
    if (user_sid) {
      add_full_control(user_sid, TRUSTEE_IS_UNKNOWN);
    }

    PACL new_dacl = nullptr;
    if (SetEntriesInAclW(entry_count, entries, nullptr, &new_dacl) != ERROR_SUCCESS || !new_dacl) {
      return false;
    }
    auto free_dacl = util::fail_guard([&new_dacl]() {
      if (new_dacl) {
        LocalFree(new_dacl);
      }
    });

    const DWORD info = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    DWORD status = SetNamedSecurityInfoW(
      const_cast<LPWSTR>(wide_path.c_str()),
      SE_FILE_OBJECT,
      info,
      nullptr,
      nullptr,
      new_dacl,
      nullptr
    );
    if (status != ERROR_SUCCESS && take_ownership_as_system(dir)) {
      status = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(wide_path.c_str()),
        SE_FILE_OBJECT,
        info,
        nullptr,
        nullptr,
        new_dacl,
        nullptr
      );
    }
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "statefile: failed to harden private directory "sv << dir.string()
                         << " (error="sv << status << ')';
      return false;
    }

    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    PACL actual_dacl = nullptr;
    const DWORD verify_status = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(wide_path.c_str()),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      &actual_dacl,
      nullptr,
      &security_descriptor
    );
    auto free_security_descriptor = util::fail_guard([&security_descriptor]() {
      if (security_descriptor) {
        LocalFree(security_descriptor);
      }
    });
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION acl_info {};
    bool verified = verify_status == ERROR_SUCCESS &&
                    security_descriptor && actual_dacl &&
                    GetSecurityDescriptorControl(security_descriptor, &control, &revision) &&
                    (control & SE_DACL_PROTECTED) != 0 &&
                    GetAclInformation(actual_dacl, &acl_info, sizeof(acl_info), AclSizeInformation) &&
                    acl_info.AceCount == entry_count;
    if (verified) {
      PSID expected_sids[3] {system_sid, admins_sid, user_sid};
      bool matched_sids[3] {};
      constexpr BYTE expected_ace_flags = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
      for (DWORD ace_index = 0; ace_index < acl_info.AceCount && verified; ++ace_index) {
        void *raw_ace = nullptr;
        if (!GetAce(actual_dacl, ace_index, &raw_ace) || !raw_ace) {
          verified = false;
          break;
        }
        const auto *allowed_ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
        if (allowed_ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            allowed_ace->Header.AceFlags != expected_ace_flags ||
            allowed_ace->Mask != FILE_ALL_ACCESS) {
          verified = false;
          break;
        }

        PSID ace_sid = const_cast<DWORD *>(&allowed_ace->SidStart);
        if (!IsValidSid(ace_sid)) {
          verified = false;
          break;
        }
        bool matched = false;
        for (ULONG expected_index = 0; expected_index < entry_count; ++expected_index) {
          if (!matched_sids[expected_index] && expected_sids[expected_index] &&
              EqualSid(ace_sid, expected_sids[expected_index])) {
            matched_sids[expected_index] = true;
            matched = true;
            break;
          }
        }
        verified = matched;
      }
      for (ULONG expected_index = 0; expected_index < entry_count && verified; ++expected_index) {
        verified = matched_sids[expected_index];
      }
    }
    if (!verified) {
      BOOST_LOG(warning) << "statefile: private directory ACL verification failed for "sv << dir.string()
                         << " (error="sv << verify_status << ')';
      return false;
    }

    BOOST_LOG(debug) << "statefile: hardened private directory "sv << dir.string();
    return true;
#else
    (void) path;
    return true;
#endif
  }

  void repair_config_permissions() {
#ifdef _WIN32
    std::set<fs::path> config_roots;
    std::set<fs::path> config_files;

    add_root_from_file(config_roots, config::nvhttp.file_state);
    add_root_from_file(config_roots, config::nvhttp.vibeshine_file_state);

    add_file_if_in_root(config_files, config_roots, config::sunshine.config_file);
    add_file_if_in_root(config_files, config_roots, config::stream.file_apps);
    add_file_if_in_root(config_files, config_roots, config::nvhttp.file_state);
    add_file_if_in_root(config_files, config_roots, config::nvhttp.vibeshine_file_state);
    add_file_if_in_root(config_files, config_roots, config::sunshine.credentials_file);

    static constexpr std::string_view known_config_files[] {
      "sunshine_state.json"sv,
      "vibeshine_state.json"sv,
      "sunshine.conf"sv,
      "apps.json"sv,
    };

    for (const auto &dir : config_roots) {
      enable_acl_inheritance(dir);
      for (const auto name : known_config_files) {
        config_files.insert(dir / std::string {name});
      }
    }

    for (const auto &path : config_files) {
      enable_acl_inheritance(path);
    }

    // Re-harden the private credentials directory (host TLS key) so that re-enabling
    // inheritance above cannot cascade a Users:(RX) ACE into it, and so an upgrade
    // that skipped the installer's icacls hardening is still secured at startup.
    for (const auto &dir : config_roots) {
      (void) secure_private_directory((dir / "credentials").string());
    }
#endif
  }

  void migrate_recent_state_keys() {
    std::call_once(migration_once, [] {
      const fs::path old_path = sunshine_state_path();
      const fs::path new_path = vibeshine_state_path();

      if (old_path.empty() || new_path.empty() || old_path == new_path) {
        return;
      }

      std::lock_guard<std::mutex> guard(state_mutex());

      pt::ptree old_tree;
      const auto old_load_result = load_tree_for_update(old_path, old_tree);
      if (old_load_result == json_load_result_e::failed) {
        return;
      }

      pt::ptree new_tree;
      const auto new_load_result = load_tree_for_update(new_path, new_tree);
      if (new_load_result == json_load_result_e::failed) {
        return;
      }

      bool old_modified = false;
      bool new_modified = false;

      if (old_load_result == json_load_result_e::loaded) {
        auto old_root_it = old_tree.find("root");
        if (old_root_it != old_tree.not_found()) {
          auto &old_root = old_root_it->second;

          auto move_child = [&](const std::string &key) {
            auto child_it = old_root.find(key);
            if (child_it == old_root.not_found()) {
              return;
            }
            auto &new_root = ensure_root(new_tree);
            if (new_root.find(key) == new_root.not_found()) {
              new_root.put_child(key, child_it->second);
              new_modified = true;
            }
            old_root.erase(old_root.to_iterator(child_it));
            old_modified = true;
          };

          move_child("api_tokens");
          move_child("session_tokens");

          if (auto last = old_root.get_optional<std::string>("last_notified_version")) {
            auto &new_root = ensure_root(new_tree);
            if (!new_root.get_optional<std::string>("last_notified_version")) {
              new_root.put("last_notified_version", *last);
              new_modified = true;
            }
            old_root.erase("last_notified_version");
            old_modified = true;
          }
        }
      }

      if (new_modified) {
        try {
          write_tree(new_path, new_tree);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "statefile: failed to write "sv << new_path.string() << ": "sv << e.what();
        }
      }
      if (old_modified) {
        try {
          write_tree(old_path, old_tree);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "statefile: failed to write "sv << old_path.string() << ": "sv << e.what();
        }
      }
    });
  }

  bool share_state_file() {
    const auto &sunshine = sunshine_state_path();
    const auto &vibeshine = vibeshine_state_path();

    if (sunshine.empty() || vibeshine.empty()) {
      return false;
    }

    if (sunshine == vibeshine) {
      return true;
    }

    try {
      const fs::path sunshine_path {sunshine};
      const fs::path vibeshine_path {vibeshine};

      if (
        fs::exists(sunshine_path) &&
        fs::exists(vibeshine_path) &&
        fs::equivalent(sunshine_path, vibeshine_path)
      ) {
        return true;
      }

#ifdef _WIN32
      auto normalize_case = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
          return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
      };

      auto normalized_sunshine = normalize_case(sunshine_path.lexically_normal().native());
      auto normalized_vibeshine = normalize_case(vibeshine_path.lexically_normal().native());

      if (normalized_sunshine == normalized_vibeshine) {
        return true;
      }
#else
      if (sunshine_path.lexically_normal() == vibeshine_path.lexically_normal()) {
        return true;
      }
#endif
    } catch (const std::exception &) {
    }

    return false;
  }
  void save_snapshot_exclude_devices(const std::vector<std::string> &devices) {
    migrate_recent_state_keys();
    const auto &path_str = vibeshine_state_path();
    if (path_str.empty()) {
      BOOST_LOG(warning) << "statefile: cannot save snapshot exclusions - vibeshine state path is empty";
      return;
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (load_tree_for_update(path, root) == json_load_result_e::failed) {
      return;
    }

    // Build the exclusion list as a property tree array
    pt::ptree exclusions_pt;
    for (const auto &device_id : devices) {
      if (!device_id.empty()) {
        pt::ptree item;
        item.put_value(device_id);
        exclusions_pt.push_back({"", item});
      }
    }

    auto &root_node = ensure_root(root);
    root_node.put_child("snapshot_exclude_devices", exclusions_pt);

    try {
      write_tree(path, root);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "statefile: failed to write "sv << path.string() << ": "sv << e.what();
      return;
    }
    BOOST_LOG(info) << "statefile: persisted " << devices.size() << " snapshot exclusion device(s) to vibeshine state";
  }

  std::vector<std::string> load_snapshot_exclude_devices() {
    migrate_recent_state_keys();
    const auto &path_str = vibeshine_state_path();
    if (path_str.empty()) {
      return {};
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (!load_tree_if_exists(path, root)) {
      return {};
    }

    std::vector<std::string> devices;
    try {
      auto root_node_opt = root.get_child_optional("root");
      if (!root_node_opt) {
        return {};
      }
      auto exclusions_opt = root_node_opt->get_child_optional("snapshot_exclude_devices");
      if (!exclusions_opt) {
        return {};
      }
      for (const auto &item : *exclusions_opt) {
        const auto device_id = item.second.get_value<std::string>("");
        if (!device_id.empty()) {
          devices.push_back(device_id);
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "statefile: failed to parse snapshot exclusions: " << e.what();
    }
    return devices;
  }

  void remember_virtual_display_device(const std::string &device_id) {
    if (device_id.empty()) {
      return;
    }
    migrate_recent_state_keys();
    const auto &path_str = vibeshine_state_path();
    if (path_str.empty()) {
      return;
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (load_tree_for_update(path, root) == json_load_result_e::failed) {
      return;
    }

    auto &root_node = ensure_root(root);
    std::vector<std::string> devices;
    if (auto devices_opt = root_node.get_child_optional("virtual_display_devices")) {
      for (const auto &item : *devices_opt) {
        const auto id = item.second.get_value<std::string>("");
        if (!id.empty()) {
          devices.push_back(id);
        }
      }
    }

    const auto ascii_lower = [](char ch) {
      return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    };
    const auto equals_ci = [&](const std::string &lhs, const std::string &rhs) {
      return lhs.size() == rhs.size() &&
             std::equal(lhs.begin(), lhs.end(), rhs.begin(), [&](char a, char b) {
               return ascii_lower(a) == ascii_lower(b);
             });
    };
    for (const auto &existing : devices) {
      if (equals_ci(existing, device_id)) {
        return;  // already remembered; avoid rewriting the state file
      }
    }

    devices.push_back(device_id);
    constexpr size_t kMaxRememberedVirtualDisplays = 16;
    if (devices.size() > kMaxRememberedVirtualDisplays) {
      devices.erase(devices.begin(), devices.end() - kMaxRememberedVirtualDisplays);
    }

    pt::ptree devices_pt;
    for (const auto &id : devices) {
      pt::ptree item;
      item.put_value(id);
      devices_pt.push_back({"", item});
    }
    root_node.put_child("virtual_display_devices", devices_pt);

    try {
      write_tree(path, root);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "statefile: failed to write "sv << path.string() << ": "sv << e.what();
      return;
    }
    BOOST_LOG(info) << "statefile: remembered virtual display device " << device_id
                    << " (" << devices.size() << " total) in vibeshine state";
  }

  std::vector<std::string> load_virtual_display_devices() {
    migrate_recent_state_keys();
    const auto &path_str = vibeshine_state_path();
    if (path_str.empty()) {
      return {};
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (!load_tree_if_exists(path, root)) {
      return {};
    }

    std::vector<std::string> devices;
    try {
      auto root_node_opt = root.get_child_optional("root");
      if (!root_node_opt) {
        return {};
      }
      auto devices_opt = root_node_opt->get_child_optional("virtual_display_devices");
      if (!devices_opt) {
        return {};
      }
      for (const auto &item : *devices_opt) {
        const auto device_id = item.second.get_value<std::string>("");
        if (!device_id.empty()) {
          devices.push_back(device_id);
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "statefile: failed to parse virtual display devices: " << e.what();
    }
    return devices;
  }

  void save_display_helper_engine(const std::string &engine) {
    if (engine.empty()) {
      return;
    }
    migrate_recent_state_keys();
    const auto &path_str = vibeshine_state_path();
    if (path_str.empty()) {
      return;
    }

    std::lock_guard<std::mutex> guard(state_mutex());
    const fs::path path(path_str);

    pt::ptree root;
    if (load_tree_for_update(path, root) == json_load_result_e::failed) {
      return;
    }

    auto &root_node = ensure_root(root);
    if (root_node.get<std::string>("display_helper_engine", "") == engine) {
      return;  // unchanged; avoid rewriting the state file
    }
    root_node.put("display_helper_engine", engine);

    try {
      write_tree(path, root);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "statefile: failed to write "sv << path.string() << ": "sv << e.what();
      return;
    }
    BOOST_LOG(info) << "statefile: persisted display helper engine '" << engine << "' to vibeshine state";
  }

}  // namespace statefile

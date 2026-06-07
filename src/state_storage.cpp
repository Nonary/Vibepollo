#include "state_storage.h"

#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "utility.h"

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cwctype>
#include <filesystem>
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
      loaded,
      missing,
      failed,
    };

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

      try {
        pt::ptree parsed;
        pt::read_json(path.string(), parsed);
        out = std::move(parsed);
        return json_load_result_e::loaded;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "statefile: refusing to update "sv << path.string()
                         << " after failed read: "sv << e.what();
        return json_load_result_e::failed;
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

    bool enable_acl_inheritance(const fs::path &path) {
      if (!path_exists_or_may_be_access_denied(path)) {
        return false;
      }

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

}  // namespace statefile

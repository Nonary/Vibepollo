#include "state_storage.h"

#include "config.h"
#include "logging.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>

using namespace std::literals;

namespace statefile {
  namespace {
    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;

    std::once_flag migration_once;

    pt::ptree &ensure_root(pt::ptree &tree) {
      auto it = tree.find("root");
      if (it == tree.not_found()) {
        auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
        return inserted->second;
      }
      return it->second;
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
      try {
        if (!path.empty()) {
          auto dir = path;
          dir.remove_filename();
          if (!dir.empty() && !fs::exists(dir)) {
            fs::create_directories(dir);
          }
        }
        pt::write_json(path.string(), tree);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "statefile: failed to write "sv << path.string() << ": "sv << e.what();
      }
    }
  }  // namespace

  std::mutex &state_mutex() {
    static std::mutex mutex;
    return mutex;
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

  void migrate_recent_state_keys() {
    std::call_once(migration_once, [] {
      const fs::path old_path = sunshine_state_path();
      const fs::path new_path = vibeshine_state_path();

      if (old_path.empty() || new_path.empty() || old_path == new_path) {
        return;
      }

      std::lock_guard<std::mutex> guard(state_mutex());

      pt::ptree old_tree;
      const bool old_loaded = load_tree_if_exists(old_path, old_tree);

      pt::ptree new_tree;
      const bool new_loaded = load_tree_if_exists(new_path, new_tree);
      (void) new_loaded;

      bool old_modified = false;
      bool new_modified = false;

      if (old_loaded) {
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
        write_tree(new_path, new_tree);
      }
      if (old_modified) {
        write_tree(old_path, old_tree);
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
    (void) load_tree_if_exists(path, root);

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

    write_tree(path, root);
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

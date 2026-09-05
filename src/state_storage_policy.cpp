/**
 * @file src/state_storage_policy.cpp
 * @brief Callback-driven JSON state-file recovery and write policy.
 */

#include "state_storage_policy.h"

#include <boost/property_tree/json_parser.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace statefile::policy {
  namespace {
    namespace pt = boost::property_tree;
    using namespace std::literals;

    bool parses_as_json(const std::string &contents) {
      try {
        pt::ptree parsed;
        std::istringstream in(contents);
        pt::read_json(in, parsed);
        return true;
      } catch (...) {
        return false;
      }
    }
  }  // namespace

  load_result_e load_json_for_update(
    const std::string &path,
    pt::ptree &tree,
    const read_file_t &read_file,
    const quarantine_file_t &quarantine_file) {
    tree = {};
    if (path.empty() || !read_file) {
      return load_result_e::failed;
    }

    auto result = read_file(path);
    if (result.status == read_status_e::missing) {
      return load_result_e::missing;
    }
    if (result.status != read_status_e::loaded) {
      return load_result_e::failed;
    }

    auto &contents = result.contents;
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
      contents.erase(0, 3);
    }
    if (contents.find_first_not_of(" \t\r\n\f\v"sv) == std::string::npos) {
      return load_result_e::missing;
    }

    try {
      std::istringstream in(contents);
      pt::read_json(in, tree);
      return load_result_e::loaded;
    } catch (...) {
      tree = {};
      if (quarantine_file) {
        quarantine_file(path);
      }
      return load_result_e::corrupt;
    }
  }

  load_result_e load_json_for_read(
    const std::string &path,
    pt::ptree &tree,
    const read_file_t &read_file) {
    tree = {};
    if (path.empty() || !read_file) {
      return load_result_e::failed;
    }

    auto result = read_file(path);
    if (result.status == read_status_e::missing) {
      return load_result_e::missing;
    }
    if (result.status != read_status_e::loaded) {
      return load_result_e::failed;
    }

    auto &contents = result.contents;
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
      contents.erase(0, 3);
    }
    if (contents.find_first_not_of(" \t\r\n\f\v"sv) == std::string::npos) {
      // An existing but empty state file is not a new profile during startup.
      // Treating it as missing would allow the caller to mint a replacement
      // identity when the last snapshot was merely truncated or unreadable.
      return load_result_e::corrupt;
    }

    try {
      std::istringstream in(contents);
      pt::read_json(in, tree);
      return load_result_e::loaded;
    } catch (...) {
      tree = {};
      return load_result_e::corrupt;
    }
  }

  namespace {
    // Property-tree represents empty JSON arrays/objects as empty data nodes.
    // Accept that historical representation, but reject scalar roots, duplicate
    // object keys and malformed token containers before selecting a snapshot.
    bool valid_vibeshine_tree(const pt::ptree &tree) {
      const auto object = [](const pt::ptree &node) {
        if (!node.data().empty()) {
          return false;
        }
        for (const auto &[key, value] : node) {
          if (key.empty() || node.count(key) != 1) {
            return false;
          }
        }
        return true;
      };
      const auto root = tree.get_child_optional("root");
      if (!object(tree) || !root || !object(*root)) {
        return false;
      }
      for (const auto key : {"api_tokens", "session_tokens"}) {
        const auto tokens = root->get_child_optional(key);
        if (!tokens) {
          continue;
        }
        if (!tokens->data().empty()) {
          return false;
        }
        for (const auto &[entry_key, token] : *tokens) {
          const auto hash = token.get_child_optional("hash");
          if (!entry_key.empty() || !object(token) || !hash || !hash->empty() || hash->data().empty()) {
            return false;
          }
          for (const auto field : {"username", "refresh_token_hash", "rotation_id", "user_agent", "remote_address", "device_label"}) {
            if (const auto value = token.get_child_optional(field); value && !value->empty()) {
              return false;
            }
          }
          for (const auto field : {"created_at", "expires_at", "refresh_expires_at", "last_seen"}) {
            if (const auto value = token.get_child_optional(field); value && (!value->empty() || !value->get_value_optional<std::int64_t>())) {
              return false;
            }
          }
          if (const auto value = token.get_child_optional("remember_me"); value && (!value->empty() || !value->get_value_optional<bool>())) {
            return false;
          }
          if (const auto scopes = token.get_child_optional("scopes")) {
            if (!scopes->data().empty()) {
              return false;
            }
            for (const auto &[scope_key, scope] : *scopes) {
              const auto path = scope.get_child_optional("path");
              const auto methods = scope.get_child_optional("methods");
              if (!scope_key.empty() || !object(scope) || !path || !path->empty() || path->data().empty() || !methods || !methods->data().empty()) {
                return false;
              }
              for (const auto &[method_key, method] : *methods) {
                if (!method_key.empty() || !method.empty() || method.data().empty()) {
                  return false;
                }
              }
            }
          }
        }
      }
      return true;
    }
  }  // namespace

  load_result_e load_vibeshine_state(
    const std::string &path,
    pt::ptree &tree,
    const read_file_t &read_file,
    const write_file_t &write_file
  ) {
    const auto primary = load_json_for_read(path, tree, read_file);
    if (primary == load_result_e::failed) {
      return load_result_e::failed;
    }
    pt::ptree backup_tree;
    const auto backup = load_json_for_read(path + ".bak", backup_tree, read_file);
    if (primary == load_result_e::loaded && valid_vibeshine_tree(tree)) {
      // Seed recovery on upgrade as well as on writes. Never recover an older
      // snapshot over a readable, valid primary (including token revocations).
      if (backup != load_result_e::loaded || tree != backup_tree) {
        if (backup == load_result_e::failed) {
          return load_result_e::loaded;
        }
        try {
          write_json_atomic(path + ".bak", tree, write_file, read_file);
        } catch (...) {
          // A valid primary remains usable even if backup storage is unavailable.
        }
      }
      return load_result_e::loaded;
    }
    tree = {};
    if (primary == load_result_e::missing && backup == load_result_e::missing) {
      return load_result_e::missing;
    }
    if (backup != load_result_e::loaded || !valid_vibeshine_tree(backup_tree)) {
      return load_result_e::failed;
    }
    try {
      // Restore before consumers see recovered state. Do not rotate the damaged
      // primary into the backup or destroy the only usable snapshot on failure.
      write_json_atomic(path, backup_tree, write_file, read_file);
    } catch (...) {
      return load_result_e::failed;
    }
    tree = std::move(backup_tree);
    return load_result_e::loaded;
  }

  void write_vibeshine_state(
    const std::string &path,
    const pt::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file
  ) {
    if (!valid_vibeshine_tree(tree)) {
      throw std::runtime_error("refusing to write invalid auxiliary state");
    }
    write_json_atomic(path, tree, write_file, read_file);
    write_json_atomic(path + ".bak", tree, write_file, read_file);
  }

  void write_json_atomic(
    const std::string &path,
    const pt::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file) {
    if (path.empty()) {
      throw std::runtime_error("atomic JSON write path is empty");
    }
    if (!write_file || !read_file) {
      throw std::runtime_error("atomic JSON storage operations are unavailable");
    }

    std::ostringstream out;
    pt::write_json(out, tree);
    const auto contents = out.str();
    if (!parses_as_json(contents)) {
      throw std::runtime_error("refusing to write malformed JSON");
    }
    if (!write_file(path, contents)) {
      throw std::runtime_error("atomic JSON write failed");
    }

    const auto written = read_file(path);
    if (written.status != read_status_e::loaded || !parses_as_json(written.contents)) {
      throw std::runtime_error("atomic JSON write verification failed");
    }
  }
}  // namespace statefile::policy

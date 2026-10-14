/**
 * @file src/platform/windows/display_restore_policy.h
 * @brief Load host restore policy for either standalone display helper engine.
 */
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace display_helper {
  inline std::wstring restore_state_file_argument(const std::filesystem::path &path) {
    // Quote according to Windows argv rules, including trailing backslashes.
    const auto argument = L"--restore-state-file=" + path.wstring();
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const auto ch : argument) {
      if (ch == L'\\') {
        ++backslashes;
        continue;
      }
      quoted.append(backslashes * (ch == L'\"' ? 2 : 1), L'\\');
      if (ch == L'\"') quoted += L'\\';
      quoted += ch;
      backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'\"';
    return quoted;
  }

  inline std::optional<bool> read_boot_restore_prefer_golden(const std::filesystem::path &state_file) {
    auto backup_file = state_file;
    backup_file += ".bak";
    for (const auto &path : {state_file, backup_file}) {
      try {
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        boost::property_tree::ptree state;
        boost::property_tree::read_json(file, state);
        const auto state_root = state.get_child_optional("root");
        const auto object = [](const boost::property_tree::ptree &node) {
          if (!node.data().empty()) return false;
          for (const auto &[key, value] : node) {
            if (key.empty() || node.count(key) != 1) return false;
          }
          return true;
        };
        if (!object(state) || !state_root || !object(*state_root)) continue;
        const auto value = state_root->get_child_optional("display_restore_prefer_golden");
        // Property_tree reads native JSON booleans and its own serialized boolean
        // strings identically. Accept only those scalars, never arrays or objects.
        if (value && value->empty()) {
          if (value->data() == "true") return true;
          if (value->data() == "false") return false;
        }
        // A valid primary is authoritative, including default/invalid policy.
        // Never revive an old preference from backup or another directory.
        return true;
      } catch (const std::exception &) {
        // Match host recovery: a missing/corrupt primary can use its backup.
      }
    }
    return std::nullopt;
  }

  inline bool load_boot_restore_prefer_golden(
    const std::vector<std::filesystem::path> &config_roots,
    const std::vector<std::filesystem::path> &snapshot_roots,
    const std::optional<std::filesystem::path> &state_file = std::nullopt) {
    if (state_file) {
      // An explicit custom state path must never read another host's policy.
      return read_boot_restore_prefer_golden(*state_file).value_or(true);
    }
    // Host state normally lives beside its executable, even when snapshots are
    // per-user. Consult it before old state copies in snapshot directories.
    auto roots = config_roots;
    roots.insert(roots.end(), snapshot_roots.begin(), snapshot_roots.end());
    for (const auto &root : roots) {
      if (const auto preference = read_boot_restore_prefer_golden(root / "vibeshine_state.json")) {
        return *preference;
      }
    }
    // Match dd_always_restore_from_golden's host default for older state files.
    // The existing recovery strategy retains session fallbacks when no golden exists.
    return true;
  }
}  // namespace display_helper

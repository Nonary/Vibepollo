/**
 * @file src/state_storage_migration.h
 * @brief Recent metadata migration with destination-first persistence.
 */
#pragma once

#include "update_notification_state.h"

namespace statefile::migration {
  // The caller has already selected/recovered and validated both snapshots and
  // holds the state lock. Writers preserve its existing atomic-write policy.
  template<typename WriteDestination, typename WriteSource>
  bool migrate_recent_keys(
    const boost::property_tree::ptree &source,
    const boost::property_tree::ptree &destination,
    bool source_loaded,
    WriteDestination &&write_destination,
    WriteSource &&write_source
  ) {
    if (!source_loaded) return true;
    auto next_source = source;
    auto next_destination = destination;
    const auto source_root = next_source.get_child_optional("root");
    if (!source_root) return true;
    auto &old_root = *source_root;
    bool source_modified = false;
    bool destination_modified = false;
    const auto destination_root = [&]() -> boost::property_tree::ptree & {
      const auto root = next_destination.get_child_optional("root");
      if (root) return *root;
      return next_destination.put_child("root", boost::property_tree::ptree {});
    };
    const auto move_child = [&](const std::string &key) {
      const auto child = old_root.find(key);
      if (child == old_root.not_found()) return;
      boost::property_tree::ptree &new_root = destination_root();
      if (new_root.find(key) == new_root.not_found()) {
        new_root.put_child(key, child->second);
        destination_modified = true;
      }
      old_root.erase(old_root.to_iterator(child));
      source_modified = true;
    };
    // Keep the existing destination-wins policy for authorization metadata.
    move_child("api_tokens");
    move_child("session_tokens");

    if (old_root.find("notified_versions") != old_root.not_found() || old_root.get_optional<std::string>("last_notified_version")) {
      boost::property_tree::ptree &new_root = destination_root();
      auto history = update::notification::read_root(new_root);
      const auto old_history = update::notification::read_root(old_root);
      for (const auto &version : old_history.notified_versions) update::notification::append(history, version);
      if (!new_root.get_optional<std::string>("last_notified_version")) history.last_notified_version = old_history.last_notified_version;
      update::notification::write_root(new_root, history);
      destination_modified = true;
      old_root.erase("notified_versions");
      old_root.erase("last_notified_version");
      source_modified = true;
    }

    // Failed destination persistence leaves the original source authoritative.
    // A later source failure is safe: the destination already has every moved key.
    if (destination_modified && !write_destination(next_destination)) return false;
    if (source_modified && !write_source(next_source)) return false;
    return true;
  }
}  // namespace statefile::migration

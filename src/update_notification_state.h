/**
 * @file src/update_notification_state.h
 * @brief Optional update history parsing, separate from pairing authorization.
 */
#pragma once

#include <algorithm>
#include <boost/property_tree/ptree.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace update::notification {
  struct history_t {
    std::string last_notified_version;
    std::vector<std::string> notified_versions;
  };

  inline void append(history_t &history, const std::string &version) {
    if (!version.empty() && std::find(history.notified_versions.begin(), history.notified_versions.end(), version) == history.notified_versions.end()) {
      history.notified_versions.push_back(version);
    }
  }

  inline history_t read_root(const nlohmann::json &root) {
    history_t history;
    if (!root.is_object()) return history;
    const auto last = root.find("last_notified_version");
    if (last != root.end() && last->is_string()) history.last_notified_version = last->get<std::string>();
    const auto versions = root.find("notified_versions");
    // Missing, scalar, and the old property-tree empty-array string are optional
    // metadata, never reasons to reject an authorized pairing snapshot.
    if (versions != root.end() && versions->is_array()) {
      for (const auto &version : *versions) {
        if (version.is_string()) append(history, version.get<std::string>());
      }
    }
    append(history, history.last_notified_version);
    return history;
  }

  inline history_t read_root(const boost::property_tree::ptree &root) {
    history_t history;
    if (const auto last = root.get_optional<std::string>("last_notified_version")) history.last_notified_version = *last;
    if (const auto versions = root.get_child_optional("notified_versions")) {
      for (const auto &version : *versions) {
        if (version.first.empty() && version.second.empty()) append(history, version.second.data());
      }
    }
    append(history, history.last_notified_version);
    return history;
  }

  inline history_t read_file(const boost::property_tree::ptree &tree) {
    const auto root = tree.get_child_optional("root");
    return root ? read_root(*root) : history_t {};
  }

  inline void write_root(nlohmann::json &root, const history_t &history) {
    root["last_notified_version"] = history.last_notified_version;
    root["notified_versions"] = history.notified_versions;
  }

  inline void write_root(boost::property_tree::ptree &root, const history_t &history) {
    root.put("last_notified_version", history.last_notified_version);
    boost::property_tree::ptree versions;
    for (const auto &version : history.notified_versions) {
      boost::property_tree::ptree entry;
      entry.put("", version);
      versions.push_back({"", entry});
    }
    root.put_child("notified_versions", versions);
  }

  template<typename State>
  history_t snapshot(State &state) {
    std::lock_guard<std::mutex> lock(state.notification_mutex);
    return {state.last_notified_version, state.notified_versions};
  }

  template<typename State>
  void apply(State &state, history_t history) {
    std::lock_guard<std::mutex> lock(state.notification_mutex);
    state.last_notified_version = std::move(history.last_notified_version);
    state.notified_versions = std::move(history.notified_versions);
  }
}  // namespace update::notification

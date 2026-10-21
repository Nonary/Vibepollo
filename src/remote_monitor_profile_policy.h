#pragma once

#include "remote_session.h"
#include <nlohmann/json.hpp>

namespace remote_session {
  inline bool marked_monitor_profile(const nlohmann::json &app) {
    const auto marker = app.find("remote-monitor-profile");
    return app.is_object() && marker != app.end() && marker->is_boolean() && marker->get<bool>();
  }

  inline std::string_view monitor_profile_json_error(const nlohmann::json &app, const std::vector<std::string_view> &configured_uuids) {
    if (!marked_monitor_profile(app)) return {};
    const auto uuid = app.find("uuid");
    if (uuid == app.end() || !uuid->is_string()) return "Remote Monitor profile requires a valid literal UUID.";
    return monitor_profile_uuid_error(uuid->get_ref<const std::string &>(), configured_uuids);
  }

  inline std::string_view monitor_profile_json_error(const nlohmann::json &app, const nlohmann::json &configured) {
    std::vector<std::string_view> uuids;
    if (configured.is_array()) {
      for (const auto &row : configured) {
        const auto value = row.find("uuid");
        if (row.is_object() && value != row.end() && value->is_string()) uuids.push_back(value->get_ref<const std::string &>());
      }
    }
    return monitor_profile_json_error(app, uuids);
  }
}

namespace proc {
  // Shared parser/save/GET validation; raw-file order is used for published input.
  std::string_view configured_monitor_profile_error_from_file(const nlohmann::json &app, const nlohmann::json &file_tree, std::string_view content);
  std::string_view configured_monitor_profile_error(const nlohmann::json &app, const nlohmann::json &file_tree);
  std::string_view monitor_profile_changes_error(const nlohmann::json &before, const nlohmann::json &after, std::string_view before_file = {});
}

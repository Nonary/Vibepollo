#pragma once

#include "remote_session.h"
#include <nlohmann/json.hpp>

namespace remote_session {
  inline bool marked_monitor_profile(const nlohmann::json &app) {
    const auto marker = app.find("remote-monitor-profile");
    return app.is_object() && marker != app.end() && marker->is_boolean() && marker->get<bool>();
  }

  inline std::optional<display_target::descriptor_t> monitor_target_from_json(const nlohmann::json &app) {
    const auto it = app.find("remote-monitor-target");
    if (it == app.end() || !it->is_object() || it->size() != 3 ||
        !it->contains("platform") || !it->contains("kind") || !it->contains("id") ||
        !(*it)["platform"].is_string() || !(*it)["kind"].is_string() || !(*it)["id"].is_string()) return std::nullopt;
    const auto platform = (*it)["platform"].get<std::string>();
    const auto kind = (*it)["kind"].get<std::string>();
    const auto id = (*it)["id"].get<std::string>();
    if ((platform != "linux-drm" && platform != "windows-device") ||
        (kind != "physical" && kind != "shared-virtual") || id.empty() || id.size() > 4096 ||
        id.find_first_of("\r\n\0", 0, 3) != std::string::npos) return std::nullopt;
    return display_target::descriptor_t {
      platform == "linux-drm" ? display_target::platform_e::linux_drm : display_target::platform_e::windows_device,
      kind == "physical" ? display_target::kind_e::physical : display_target::kind_e::shared_virtual, id};
  }

  inline std::string_view monitor_profile_json_error(const nlohmann::json &app, const std::vector<std::string_view> &configured_uuids) {
    if (!marked_monitor_profile(app)) return {};
    if (app.contains("remote-monitor-target") && !monitor_target_from_json(app)) return "Remote Monitor target requires literal platform, kind, and exact id fields.";
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

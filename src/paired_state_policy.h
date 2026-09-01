#pragma once

#include "state_storage_policy.h"

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <charconv>
#include <cctype>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace nvhttp::state_policy {
  inline bool valid_uuid(const std::string &value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (value[i] != '-') return false;
      } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
  }

  inline bool boolean(const nlohmann::json &value) {
    return value.is_boolean() || (value.is_number_integer() && (value == 0 || value == 1)) ||
           (value.is_string() && (value == "true" || value == "false" || value == "0" || value == "1"));
  }

  inline bool permission(const nlohmann::json &value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
    if (value.is_number_integer()) return value.get<std::int64_t>() >= 0 && value.get<std::int64_t>() <= std::numeric_limits<std::uint32_t>::max();
    if (!value.is_string()) return false;
    const auto &text = value.get_ref<const std::string &>();
    std::uint32_t parsed;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc {} && result.ptr == text.data() + text.size();
  }

  // Old property-tree writers encode empty arrays as "". Normalize only those
  // known containers; preserve every Apollo permission, command and unknown key.
  inline bool normalize_snapshot(nlohmann::json &tree) {
    if (!tree.is_object() || !tree.contains("root") || !tree["root"].is_object()) return false;
    auto &root = tree["root"];
    if (!root.contains("uniqueid") || !root["uniqueid"].is_string() || !valid_uuid(root["uniqueid"])) return false;
    for (const auto key : {"remote_display_layout", "last_notified_version"}) {
      if (root.contains(key) && !root[key].is_string()) return false;
    }
    const auto array = [](nlohmann::json &node) {
      if (node == "") node = nlohmann::json::array();
      return node.is_array();
    };
    std::unordered_set<std::string> uuids;
    std::unordered_set<std::string> certs;
    if (root.contains("named_devices")) {
      auto &devices = root["named_devices"];
      if (!array(devices) || devices.size() > 256) return false;
      for (auto &device : devices) {
        if (!device.is_object() || !device.contains("uuid") || !device["uuid"].is_string() ||
            !valid_uuid(device["uuid"]) || !uuids.insert(device["uuid"]).second ||
            !device.contains("cert") || !device["cert"].is_string() || device["cert"].get_ref<const std::string &>().empty() ||
            device["cert"].get_ref<const std::string &>().size() > 65536 || !certs.insert(device["cert"]).second) return false;
        if (device.contains("perm") && !permission(device["perm"])) return false;
        if (device.contains("enabled") && !boolean(device["enabled"])) return false;
        if (device.contains("enabled") && device["enabled"].is_number_integer()) device["enabled"] = device["enabled"] == 1;
        if (device.contains("last_seen") && device["last_seen"].is_boolean()) return false;
        for (const auto key : {"name", "display_mode", "hdr_profile", "output_name_override", "virtual_display_mode", "virtual_display_layout"}) {
          if (device.contains(key) && !device[key].is_string()) return false;
        }
        for (const auto key : {"enable_legacy_ordering", "allow_client_commands", "always_use_virtual_display", "prefer_10bit_sdr"}) {
          if (device.contains(key) && !device[key].is_null() && !boolean(device[key])) return false;
          if (device.contains(key) && device[key].is_number_integer()) device[key] = device[key] == 1;
        }
        for (const auto key : {"do", "undo"}) {
          if (!device.contains(key)) continue;
          if (!array(device[key])) return false;
          for (auto &command : device[key]) {
            if (!command.is_object() || !command.contains("cmd") || !command["cmd"].is_string() ||
                (command.contains("elevated") && !boolean(command["elevated"]))) return false;
            if (command.contains("elevated") && command["elevated"].is_number_integer()) command["elevated"] = command["elevated"] == 1;
          }
        }
        if (device.contains("config_overrides") && device["config_overrides"] == "") device["config_overrides"] = nlohmann::json::object();
        if (device.contains("config_overrides") && !device["config_overrides"].is_object()) return false;
        if (device.contains("config_overrides")) {
          for (const auto &entry : device["config_overrides"].items()) {
            if (!entry.key().empty() && !entry.value().is_string()) return false;
          }
        }
      }
    }
    if (root.contains("devices")) {
      if (!array(root["devices"])) return false;
      std::size_t legacy_count = uuids.size();
      for (auto &device : root["devices"]) {
        if (!device.is_object()) return false;
        if (!device.contains("certs")) continue;
        if (!array(device["certs"])) return false;
        for (const auto &cert : device["certs"]) {
          if (++legacy_count > 256 || !cert.is_string() || cert.get_ref<const std::string &>().empty() ||
              cert.get_ref<const std::string &>().size() > 65536 || !certs.insert(cert).second) return false;
        }
      }
    }
    return true;
  }

  // The shared property-tree policy validates structure and certificates, but
  // erases scalar types. Check the original JSON before choosing a recovery
  // snapshot so a typed-parser failure cannot bypass a usable backup.
  inline bool valid_primary_json(const std::string &contents) {
    try {
      auto typed = nlohmann::json::parse(contents);
      if (!typed.is_object()) return false;
      if (!typed.contains("root") || !typed["root"].is_object() || !typed["root"].contains("uniqueid")) return true;
      return normalize_snapshot(typed);
    } catch (...) {
      return false;
    }
  }
  inline bool valid_primary_tree(const boost::property_tree::ptree &tree, bool allow_bootstrap) {
    if (!statefile::policy::valid_primary_state(tree, allow_bootstrap)) return false;
    if (!statefile::policy::valid_primary_state(tree, false)) return true;
    std::ostringstream serialized;
    boost::property_tree::write_json(serialized, tree);
    auto typed = nlohmann::json::parse(serialized.str());
    return normalize_snapshot(typed);
  }
}  // namespace nvhttp::state_policy

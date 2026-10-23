#pragma once

#include "state_storage_policy.h"
#include "crypto.h"

#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <charconv>
#include <cctype>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

  inline bool normalize_last_seen(nlohmann::json &value) {
    if (value.is_null()) return true;
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() <= std::numeric_limits<std::int64_t>::max();
    if (value.is_number_integer()) return true;
    if (!value.is_string()) return false;
    const auto &text = value.get_ref<const std::string &>();
    if (text.empty()) return false;
    const auto digits = text.front() == '-' ? 1U : 0U;
    if (digits == text.size() || (text[digits] == '0' && text.size() - digits > 1)) return false;
    std::int64_t parsed;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()) return false;
    // Older property-tree writers persisted timestamps as decimal strings.
    value = parsed;
    return true;
  }

  // Old property-tree writers encode empty arrays as "". Normalize only those
  // known containers; preserve every Apollo permission, command and unknown key.
  // Offline duplicate inspection may retain repeated certificates. Normal
  // loading collapses 1.x duplicates into the first record that 1.x used.
  inline bool normalize_snapshot(nlohmann::json &tree, const statefile::policy::certificate_identity_t &certificate_identity = {}, bool allow_duplicate_certificates = false,
                                 std::vector<statefile::policy::collapsed_pairing_record_t> *collapsed = nullptr) {
    if (collapsed) collapsed->clear();
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
    std::vector<statefile::policy::collapsed_pairing_record_t> collapsed_records;
    const auto identity = [&](const nlohmann::json &certificate) -> std::optional<std::string> {
      const auto &pem = certificate.get_ref<const std::string &>();
      return certificate_identity ? certificate_identity(pem) : std::make_optional(pem);
    };
    if (root.contains("named_devices")) {
      auto &devices = root["named_devices"];
      if (!array(devices) || devices.size() > 256) return false;
      nlohmann::json survivors = nlohmann::json::array();
      std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> first_by_cert;
      std::size_t index = 0;
      for (auto &device : devices) {
        if (!device.is_object() || !device.contains("uuid") || !device["uuid"].is_string() || !valid_uuid(device["uuid"]) || !uuids.insert(device["uuid"]).second || !device.contains("cert") || !device["cert"].is_string() || device["cert"].get_ref<const std::string &>().empty() || device["cert"].get_ref<const std::string &>().size() > 65536) {
          return false;
        }
        const auto certificate = identity(device["cert"]);
        if (!certificate || certificate->empty()) {
          return false;
        }
        if (device.contains("perm") && !permission(device["perm"])) return false;
        if (device.contains("enabled") && !boolean(device["enabled"])) return false;
        if (device.contains("enabled") && device["enabled"].is_number_integer()) device["enabled"] = device["enabled"] == 1;
        if (device.contains("last_seen") && !normalize_last_seen(device["last_seen"])) return false;
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
        certs.insert(*certificate);
        const auto [first, inserted] = first_by_cert.emplace(*certificate, std::pair {survivors.size(), index});
        if (!inserted && !allow_duplicate_certificates) {
          // Either a revoked Apollo permission or a disabled imported Shine
          // record revokes the survivor. Never gain access by deduplicating.
          const auto denied = [](const nlohmann::json &record) {
            if (record.contains("enabled") && (record["enabled"] == false || record["enabled"] == "false" || record["enabled"] == "0")) return true;
            if (!record.contains("perm")) return false;
            std::uint32_t mask = 0;
            if (record["perm"].is_string()) {
              const auto &text = record["perm"].get_ref<const std::string &>();
              std::from_chars(text.data(), text.data() + text.size(), mask);
            } else {
              mask = record["perm"].get<std::uint32_t>();
            }
            return (mask & static_cast<std::uint32_t>(crypto::PERM::_all)) == 0;
          };
          auto &survivor = survivors[first->second.first];
          if (denied(device) || denied(survivor)) {
            survivor["perm"] = 0;
            survivor["enabled"] = false;
          }
          collapsed_records.push_back({first->second.second, survivor["uuid"].get<std::string>(), index, device["uuid"].get<std::string>()});
        } else {
          survivors.push_back(device);
        }
        ++index;
      }
      if (!allow_duplicate_certificates && !collapsed_records.empty()) devices = std::move(survivors);
    }
    if (root.contains("devices")) {
      if (!array(root["devices"])) return false;
      std::size_t legacy_count = uuids.size();
      std::unordered_set<std::string> legacy_certs;
      for (auto &device : root["devices"]) {
        if (!device.is_object()) return false;
        if (!device.contains("certs")) continue;
        if (!array(device["certs"])) return false;
        for (auto it = device["certs"].begin(); it != device["certs"].end();) {
          if (!it->is_string() || it->get_ref<const std::string &>().empty() || it->get_ref<const std::string &>().size() > 65536) {
            return false;
          }
          const auto certificate = identity(*it);
          if (!certificate || certificate->empty()) {
            return false;
          }
          // Old writers retained a legacy copy after importing this named
          // record. Keep its Apollo permissions and commands authoritative.
          if (certs.contains(*certificate)) {
            it = device["certs"].erase(it);
          } else {
            if (++legacy_count > 256) {
              return false;
            }
            if (!legacy_certs.insert(*certificate).second && !allow_duplicate_certificates) {
              it = device["certs"].erase(it);
            } else {
              ++it;
            }
          }
        }
      }
    }
    if (collapsed) *collapsed = std::move(collapsed_records);
    return true;
  }

  // The shared property-tree policy validates structure and certificates, but
  // erases scalar types. Check the original JSON before choosing a recovery
  // snapshot so a typed-parser failure cannot bypass a usable backup.
  inline bool valid_primary_json_with_certificates(const std::string &contents, const statefile::policy::certificate_identity_t &certificate_identity) {
    try {
      auto typed = nlohmann::json::parse(contents);
      if (!typed.is_object()) return false;
      if (!typed.contains("root") || !typed["root"].is_object() || !typed["root"].contains("uniqueid")) return true;
      return normalize_snapshot(typed, certificate_identity);
    } catch (...) {
      return false;
    }
  }

  inline bool valid_primary_json(const std::string &contents) {
    return valid_primary_json_with_certificates(contents, {});
  }

  // Restore only the timestamp type lost by property-tree serialization. The
  // original JSON validator still rejects malformed timestamp values before
  // selecting a snapshot; a literal string "null" is not an unset timestamp.
  inline bool primary_tree_json(const boost::property_tree::ptree &tree, nlohmann::json &typed) {
    try {
      std::ostringstream serialized;
      boost::property_tree::write_json(serialized, tree);
      typed = nlohmann::json::parse(serialized.str());
      const auto root = typed.find("root");
      if (root == typed.end() || !root->is_object()) return true;
      const auto devices = root->find("named_devices");
      if (devices == root->end() || !devices->is_array()) return true;
      for (auto &device : *devices) {
        if (!device.is_object()) continue;
        const auto last_seen = device.find("last_seen");
        if (last_seen == device.end()) continue;
        if (*last_seen == "null") *last_seen = nullptr;
        if (!normalize_last_seen(*last_seen)) return false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  inline bool valid_primary_tree(const boost::property_tree::ptree &tree, bool allow_bootstrap) {
    if (!statefile::policy::valid_primary_state(tree, allow_bootstrap)) return false;
    if (!statefile::policy::valid_primary_state(tree, false)) return true;
    nlohmann::json typed;
    return primary_tree_json(tree, typed) && normalize_snapshot(typed);
  }
}  // namespace nvhttp::state_policy

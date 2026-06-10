#include "src/platform/windows/display_helper_v2/snapshot_codec.h"

#ifdef _WIN32

  #include <algorithm>
  #include <cctype>
  #include <cstdio>
  #include <memory>

  #include <nlohmann/json.hpp>

  #include "src/logging.h"

namespace display_helper::v2::codec {
  namespace {
    std::string join_ids(const auto &items) {
      std::string out;
      bool first = true;
      for (const auto &item : items) {
        if (!first) {
          out += ", ";
        }
        first = false;
        out += item;
      }
      return out;
    }

    // Parsing helpers ported verbatim from the legacy helper.
    std::string find_str_section(const std::string &data, const std::string &key) {
      auto p = data.find("\"" + key + "\"");
      if (p == std::string::npos) {
        return {};
      }
      p = data.find(':', p);
      if (p == std::string::npos) {
        return {};
      }
      return data.substr(p + 1);
    }

    void parse_primary_field(const std::string &prim, Snapshot &snap) {
      auto q1 = prim.find('"');
      auto q2 = prim.find('"', q1 == std::string::npos ? 0 : q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
        snap.m_primary_device = prim.substr(q1 + 1, q2 - q1 - 1);
      }
    }

    void parse_topology_field(const std::string &topo_s, Snapshot &snap) {
      snap.m_topology.clear();
      size_t i = topo_s.find('[');
      if (i == std::string::npos) {
        return;
      }
      ++i;  // skip [
      while (i < topo_s.size() && topo_s[i] != ']') {
        while (i < topo_s.size() && topo_s[i] != '[' && topo_s[i] != ']') {
          ++i;
        }
        if (i >= topo_s.size() || topo_s[i] == ']') {
          break;
        }
        ++i;  // skip [
        std::vector<std::string> grp;
        while (i < topo_s.size() && topo_s[i] != ']') {
          while (i < topo_s.size() && topo_s[i] != '"' && topo_s[i] != ']') {
            ++i;
          }
          if (i >= topo_s.size() || topo_s[i] == ']') {
            break;
          }
          auto q1 = i + 1;
          auto q2 = topo_s.find('"', q1);
          if (q2 == std::string::npos) {
            break;
          }
          grp.emplace_back(topo_s.substr(q1, q2 - q1));
          i = q2 + 1;
        }
        while (i < topo_s.size() && topo_s[i] != ']') {
          ++i;
        }
        if (i < topo_s.size() && topo_s[i] == ']') {
          ++i;  // skip ]
        }
        snap.m_topology.emplace_back(std::move(grp));
      }
    }

    unsigned int parse_num_field(const std::string &obj, const char *key) {
      auto p = obj.find(key);
      if (p == std::string::npos) {
        return 0;
      }
      p = obj.find(':', p);
      if (p == std::string::npos) {
        return 0;
      }
      return static_cast<unsigned int>(std::stoul(obj.substr(p + 1)));
    }

    void parse_modes_field(const std::string &modes_s, Snapshot &snap) {
      snap.m_modes.clear();
      size_t i = modes_s.find('{');
      if (i == std::string::npos) {
        return;
      }
      ++i;
      while (i < modes_s.size() && modes_s[i] != '}') {
        while (i < modes_s.size() && modes_s[i] != '"' && modes_s[i] != '}') {
          ++i;
        }
        if (i >= modes_s.size() || modes_s[i] == '}') {
          break;
        }
        auto q1 = i + 1;
        auto q2 = modes_s.find('"', q1);
        if (q2 == std::string::npos) {
          break;
        }
        std::string id = modes_s.substr(q1, q2 - q1);
        i = modes_s.find('{', q2);
        if (i == std::string::npos) {
          break;
        }
        auto end = modes_s.find('}', i);
        if (end == std::string::npos) {
          break;
        }
        auto obj = modes_s.substr(i, end - i);
        display_device::DisplayMode dm;
        dm.m_resolution.m_width = parse_num_field(obj, "\"w\"");
        dm.m_resolution.m_height = parse_num_field(obj, "\"h\"");
        dm.m_refresh_rate.m_numerator = parse_num_field(obj, "\"num\"");
        dm.m_refresh_rate.m_denominator = parse_num_field(obj, "\"den\"");
        snap.m_modes.emplace(id, dm);
        i = end + 1;
      }
    }

    void parse_hdr_field(const std::string &hdr_s, Snapshot &snap) {
      snap.m_hdr_states.clear();
      size_t i = hdr_s.find('{');
      if (i == std::string::npos) {
        return;
      }
      ++i;
      while (i < hdr_s.size() && hdr_s[i] != '}') {
        while (i < hdr_s.size() && hdr_s[i] != '"' && hdr_s[i] != '}') {
          ++i;
        }
        if (i >= hdr_s.size() || hdr_s[i] == '}') {
          break;
        }
        auto q1 = i + 1;
        auto q2 = hdr_s.find('"', q1);
        if (q2 == std::string::npos) {
          break;
        }
        std::string id = hdr_s.substr(q1, q2 - q1);
        i = hdr_s.find(':', q2);
        if (i == std::string::npos) {
          break;
        }
        ++i;
        while (i < hdr_s.size() && (hdr_s[i] == ' ' || hdr_s[i] == '"')) {
          ++i;
        }
        std::optional<display_device::HdrState> val;
        if (hdr_s.compare(i, 2, "on") == 0) {
          val = display_device::HdrState::Enabled;
        } else if (hdr_s.compare(i, 3, "off") == 0) {
          val = display_device::HdrState::Disabled;
        } else {
          val = std::nullopt;
        }
        snap.m_hdr_states.emplace(id, val);
        while (i < hdr_s.size() && hdr_s[i] != ',' && hdr_s[i] != '}') {
          ++i;
        }
        if (i < hdr_s.size() && hdr_s[i] == ',') {
          ++i;
        }
      }
    }

    int parse_signed_num_field(const std::string &obj, const char *key) {
      auto p = obj.find(key);
      if (p == std::string::npos) {
        return 0;
      }
      p = obj.find(':', p);
      if (p == std::string::npos) {
        return 0;
      }
      return std::stoi(obj.substr(p + 1));
    }

    void parse_origins_field(const std::string &origins_s, Snapshot &snap) {
      snap.m_origins.clear();
      size_t i = origins_s.find('{');
      if (i == std::string::npos) {
        return;
      }
      ++i;
      while (i < origins_s.size() && origins_s[i] != '}') {
        while (i < origins_s.size() && origins_s[i] != '"' && origins_s[i] != '}') {
          ++i;
        }
        if (i >= origins_s.size() || origins_s[i] == '}') {
          break;
        }
        auto q1 = i + 1;
        auto q2 = origins_s.find('"', q1);
        if (q2 == std::string::npos) {
          break;
        }
        std::string id = origins_s.substr(q1, q2 - q1);
        i = origins_s.find('{', q2);
        if (i == std::string::npos) {
          break;
        }
        auto end = origins_s.find('}', i);
        if (end == std::string::npos) {
          break;
        }
        auto obj = origins_s.substr(i, end - i);
        display_device::Point pt;
        pt.m_x = parse_signed_num_field(obj, "\"x\"");
        pt.m_y = parse_signed_num_field(obj, "\"y\"");
        snap.m_origins.emplace(id, pt);
        i = end + 1;
      }
    }

    std::optional<int> parse_layout_rotation_value(const nlohmann::json &value) {
      if (value.is_number_integer()) {
        return normalize_rotation_degrees(value.get<int>());
      }
      if (value.is_string()) {
        const auto rotation_name = ascii_lower(value.get<std::string>());
        if (rotation_name == "landscape") {
          return 0;
        }
        if (rotation_name == "portrait") {
          return 90;
        }
        if (rotation_name == "landscape_flipped" || rotation_name == "landscape_inverted") {
          return 180;
        }
        if (rotation_name == "portrait_flipped" || rotation_name == "portrait_inverted") {
          return 270;
        }
      }
      if (value.is_object()) {
        if (auto it = value.find("rotation"); it != value.end()) {
          return parse_layout_rotation_value(*it);
        }
      }
      return std::nullopt;
    }

    void parse_layouts_field(
      const nlohmann::json &root,
      layout_rotation_map_t &layout_rotations,
      bool &has_layout_data) {
      layout_rotations.clear();
      has_layout_data = false;
      auto it_layouts = root.find("layouts");
      if (it_layouts == root.end() || !it_layouts->is_object()) {
        return;
      }
      has_layout_data = true;
      for (auto it = it_layouts->begin(); it != it_layouts->end(); ++it) {
        if (!it.key().empty()) {
          if (auto rotation = parse_layout_rotation_value(it.value())) {
            layout_rotations[it.key()] = *rotation;
          }
        }
      }
    }
  }  // namespace

  std::string ascii_lower(std::string s) {
    for (char &ch : s) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
    return s;
  }

  bool contains_ci(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) {
      return true;
    }
    if (haystack.size() < needle.size()) {
      return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
      bool match = true;
      for (size_t j = 0; j < needle.size(); ++j) {
        if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
            std::tolower(static_cast<unsigned char>(needle[j]))) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }
    return false;
  }

  bool equals_ci(const std::string &lhs, const std::string &rhs) {
    return lhs.size() == rhs.size() && contains_ci(lhs, rhs);
  }

  std::string normalize_device_id(std::string id) {
    id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char ch) {
               return !std::isspace(ch);
             }));
    id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char ch) {
               return !std::isspace(ch);
             }).base(),
             id.end());
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return id;
  }

  std::optional<int> normalize_rotation_degrees(int degrees) {
    int normalized = degrees % 360;
    if (normalized < 0) {
      normalized += 360;
    }
    switch (normalized) {
      case 0:
      case 90:
      case 180:
      case 270:
        return normalized;
      default:
        return std::nullopt;
    }
  }

  std::vector<std::string> flatten_topology_device_ids(const ActiveTopology &topology) {
    std::vector<std::string> ids;
    for (const auto &group : topology) {
      for (const auto &id : group) {
        if (!id.empty()) {
          ids.push_back(id);
        }
      }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  }

  std::set<std::string> snapshot_device_set(const Snapshot &s) {
    std::set<std::string> out;
    for (const auto &grp : s.m_topology) {
      for (const auto &id : grp) {
        out.insert(id);
      }
    }
    if (out.empty()) {
      for (const auto &kv : s.m_modes) {
        out.insert(kv.first);
      }
    }
    return out;
  }

  std::set<std::string> topology_device_set(const ActiveTopology &topology) {
    std::set<std::string> out;
    for (const auto &grp : topology) {
      out.insert(grp.begin(), grp.end());
    }
    return out;
  }

  ActiveTopology canonical_topology(ActiveTopology topology) {
    for (auto &group : topology) {
      std::sort(group.begin(), group.end());
    }
    std::sort(topology.begin(), topology.end());
    return topology;
  }

  bool equal_snapshots_strict(const Snapshot &a, const Snapshot &b) {
    if (canonical_topology(a.m_topology) != canonical_topology(b.m_topology)) {
      return false;
    }
    if (!(a.m_modes == b.m_modes && a.m_hdr_states == b.m_hdr_states && a.m_primary_device == b.m_primary_device)) {
      return false;
    }
    // Origins are optional for backward compatibility with older snapshots
    if (!a.m_origins.empty() && !b.m_origins.empty()) {
      return a.m_origins == b.m_origins;
    }
    return true;
  }

  std::string signature(const Snapshot &snap) {
    // Build a stable textual representation
    std::string s;
    s.reserve(1024);
    // Topology (canonical order: group enumeration order is OS-dependent and meaningless)
    s += "T:";
    auto topology = snap.m_topology;
    for (auto &grp : topology) {
      std::sort(grp.begin(), grp.end());
    }
    std::sort(topology.begin(), topology.end());
    for (const auto &grp : topology) {
      s += "[";
      for (const auto &id : grp) {
        s += id;
        s += ",";
      }
      s += "]";
    }
    // Modes
    s += ";M:";
    for (const auto &kv : snap.m_modes) {
      s += kv.first;
      s += "=";
      s += std::to_string(kv.second.m_resolution.m_width);
      s += "x";
      s += std::to_string(kv.second.m_resolution.m_height);
      s += "@";
      s += std::to_string(kv.second.m_refresh_rate.m_numerator);
      s += "/";
      s += std::to_string(kv.second.m_refresh_rate.m_denominator);
      s += ";";
    }
    // HDR
    s += ";H:";
    for (const auto &kh : snap.m_hdr_states) {
      s += kh.first;
      s += "=";
      // Avoid ambiguous null; use explicit string for readability
      if (!kh.second.has_value()) {
        s += "unknown";
      } else {
        s += (*kh.second == display_device::HdrState::Enabled) ? "on" : "off";
      }
      s += ";";
    }
    // Primary
    s += ";P:";
    s += snap.m_primary_device;
    // Origins
    s += ";O:";
    for (const auto &ko : snap.m_origins) {
      s += ko.first;
      s += "=";
      s += std::to_string(ko.second.m_x);
      s += ",";
      s += std::to_string(ko.second.m_y);
      s += ";";
    }
    return s;
  }

  bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {
    if (contains_ci(device.m_device_id, "SunshineVirtualDisplay") ||
        contains_ci(device.m_device_id, "Sunshine Virtual Display") ||
        contains_ci(device.m_display_name, "SunshineVirtualDisplay") ||
        contains_ci(device.m_display_name, "Sunshine Virtual Display") ||
        contains_ci(device.m_friendly_name, "SunshineVirtualDisplay") ||
        contains_ci(device.m_friendly_name, "Sunshine Virtual Display")) {
      return true;
    }

    if (equals_ci(device.m_friendly_name, "Sunshine Virtual Display Driver")) {
      return true;
    }

    // SDD = bundled Sunshine virtual display driver, SMK = SudoVDA (legacy driver).
    return device.m_edid &&
           (equals_ci(device.m_edid->m_manufacturer_id, "SDD") ||
            equals_ci(device.m_edid->m_manufacturer_id, "SMK"));
  }

  bool is_active_display_device(const display_device::EnumeratedDevice &device) {
    return device.m_info.has_value() || !device.m_display_name.empty();
  }

  std::string serialize_snapshot(const Snapshot &snap, const layout_rotation_map_t &layout_rotations) {
    std::string out;
    out += "{\n  \"snapshot_version\": " + std::to_string(kSnapshotLayoutVersionLatest) + ",\n  \"topology\": [";
    for (size_t i = 0; i < snap.m_topology.size(); ++i) {
      const auto &grp = snap.m_topology[i];
      out += "[";
      for (size_t j = 0; j < grp.size(); ++j) {
        out += "\"" + grp[j] + "\"";
        if (j + 1 < grp.size()) {
          out += ",";
        }
      }
      out += "]";
      if (i + 1 < snap.m_topology.size()) {
        out += ",";
      }
    }
    out += "],\n  \"modes\": {";
    size_t k = 0;
    for (const auto &kv : snap.m_modes) {
      out += "\n    \"" + kv.first + "\": { \"w\": " + std::to_string(kv.second.m_resolution.m_width) + ", \"h\": " + std::to_string(kv.second.m_resolution.m_height) + ", \"num\": " + std::to_string(kv.second.m_refresh_rate.m_numerator) + ", \"den\": " + std::to_string(kv.second.m_refresh_rate.m_denominator) + " }";
      if (++k < snap.m_modes.size()) {
        out += ",";
      }
    }
    out += "\n  },\n  \"hdr\": {";
    k = 0;
    for (const auto &kh : snap.m_hdr_states) {
      out += "\n    \"" + kh.first + "\": ";
      if (!kh.second.has_value()) {
        out += "null";
      } else {
        out += (*kh.second == display_device::HdrState::Enabled) ? "\"on\"" : "\"off\"";
      }
      if (++k < snap.m_hdr_states.size()) {
        out += ",";
      }
    }
    out += "\n  },\n  \"primary\": \"" + snap.m_primary_device + "\",\n  \"origins\": {";
    k = 0;
    for (const auto &ko : snap.m_origins) {
      out += "\n    \"" + ko.first + "\": { \"x\": " + std::to_string(ko.second.m_x) + ", \"y\": " + std::to_string(ko.second.m_y) + " }";
      if (++k < snap.m_origins.size()) {
        out += ",";
      }
    }
    out += "\n  },\n  \"layouts\": {";
    k = 0;
    for (const auto &layout : layout_rotations) {
      out += "\n    \"" + layout.first + "\": { \"rotation\": " + std::to_string(layout.second) + " }";
      if (++k < layout_rotations.size()) {
        out += ",";
      }
    }
    out += "\n  }\n}";
    return out;
  }

  ParsedSnapshot parse_snapshot_text(const std::string &data) {
    ParsedSnapshot loaded;

    try {
      auto j = nlohmann::json::parse(data, nullptr, false);
      if (j.is_object()) {
        if (j.contains("snapshot_version") && j["snapshot_version"].is_number_integer()) {
          loaded.snapshot_version = std::max(1, j["snapshot_version"].get<int>());
        }
        parse_layouts_field(j, loaded.layout_rotations, loaded.has_layout_data);
      }
    } catch (...) {
    }

    const auto prim = find_str_section(data, "primary");
    const auto topo_s = find_str_section(data, "topology");
    const auto modes_s = find_str_section(data, "modes");
    const auto hdr_s = find_str_section(data, "hdr");
    parse_primary_field(prim, loaded.snapshot);
    parse_topology_field(topo_s, loaded.snapshot);
    parse_modes_field(modes_s, loaded.snapshot);
    parse_hdr_field(hdr_s, loaded.snapshot);
    const auto origins_s = find_str_section(data, "origins");
    parse_origins_field(origins_s, loaded.snapshot);

    return loaded;
  }

  bool snapshot_text_has_restore_payload(const std::string &data) {
    try {
      Snapshot snap;
      parse_topology_field(find_str_section(data, "topology"), snap);
      parse_modes_field(find_str_section(data, "modes"), snap);
      return !snap.m_topology.empty() && !snap.m_modes.empty();
    } catch (...) {
      return false;
    }
  }

  std::optional<Snapshot> filter_snapshot_for_save(
    Snapshot snap,
    const EnumeratedDeviceList &devices,
    const std::vector<std::string> &exclusions,
    std::string &reject_reason) {
    reject_reason.clear();

    std::vector<std::string> exclusions_norm;
    exclusions_norm.reserve(exclusions.size());
    for (auto id : exclusions) {
      exclusions_norm.push_back(normalize_device_id(std::move(id)));
    }

    auto is_excluded = [&](const std::string &device_id) {
      if (exclusions_norm.empty()) {
        return false;
      }
      const auto norm = normalize_device_id(device_id);
      return std::find(exclusions_norm.begin(), exclusions_norm.end(), norm) != exclusions_norm.end();
    };

    if (snap.m_modes.empty()) {
      reject_reason = "mode set is empty";
      return std::nullopt;
    }

    // Filter out devices without display_name. These are not safe restore
    // targets and are intentionally excluded from persisted snapshots.
    std::set<std::string> valid_device_ids;
    std::vector<std::string> enumerated_devices;
    std::vector<std::string> virtual_devices;
    for (const auto &d : devices) {
      const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
      if (!id.empty()) {
        std::string detail = id;
        detail += "(display_name=";
        detail += d.m_display_name.empty() ? "<empty>" : d.m_display_name;
        detail += ")";
        enumerated_devices.push_back(std::move(detail));
      }
      if (is_virtual_display_device(d)) {
        if (is_active_display_device(d) && !id.empty()) {
          virtual_devices.push_back(id);
        }
        continue;
      }
      if (!d.m_display_name.empty()) {
        if (!id.empty()) {
          valid_device_ids.insert(id);
        }
      }
    }
    if (!virtual_devices.empty()) {
      BOOST_LOG(warning) << "Skipping display snapshot save; active virtual display device(s) are present: ["
                         << join_ids(virtual_devices) << "]";
      reject_reason = "active virtual display present";
      return std::nullopt;
    }

    if (!exclusions.empty()) {
      std::set<std::string> filtered_ids;
      std::vector<std::string> excluded_now;
      for (const auto &id : valid_device_ids) {
        if (is_excluded(id)) {
          excluded_now.push_back(id);
          continue;
        }
        filtered_ids.insert(id);
      }
      if (!excluded_now.empty()) {
        BOOST_LOG(info) << "Display snapshot: excluding devices from snapshot: [" << join_ids(excluded_now) << "]";
      }
      valid_device_ids.swap(filtered_ids);
      if (valid_device_ids.empty()) {
        reject_reason = "all devices are excluded";
        return std::nullopt;
      }
    }

    // Filter topology groups to devices with a restore-capable display_name.
    ActiveTopology filtered_topology;
    for (const auto &grp : snap.m_topology) {
      std::vector<std::string> filtered_grp;
      for (const auto &device_id : grp) {
        if (valid_device_ids.count(device_id)) {
          filtered_grp.push_back(device_id);
        }
      }
      if (!filtered_grp.empty()) {
        filtered_topology.push_back(std::move(filtered_grp));
      }
    }

    if (filtered_topology.empty()) {
      reject_reason = "no devices with valid display_name";
      if (!enumerated_devices.empty()) {
        BOOST_LOG(debug) << "Display snapshot save rejected details: enumerated_devices=[" << join_ids(enumerated_devices) << "]";
      }
      return std::nullopt;
    }

    // Update snapshot with filtered data
    snap.m_topology = std::move(filtered_topology);

    // Filter modes and hdr_states to only include valid devices
    for (auto it = snap.m_modes.begin(); it != snap.m_modes.end();) {
      if (!valid_device_ids.count(it->first)) {
        it = snap.m_modes.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = snap.m_hdr_states.begin(); it != snap.m_hdr_states.end();) {
      if (!valid_device_ids.count(it->first)) {
        it = snap.m_hdr_states.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = snap.m_origins.begin(); it != snap.m_origins.end();) {
      if (!valid_device_ids.count(it->first)) {
        it = snap.m_origins.erase(it);
      } else {
        ++it;
      }
    }

    // Clear primary if it was filtered out
    if (!valid_device_ids.count(snap.m_primary_device)) {
      snap.m_primary_device.clear();
    }

    return snap;
  }

  std::optional<ParsedSnapshot> filter_loaded_snapshot(
    ParsedSnapshot loaded,
    const EnumeratedDeviceList &devices,
    const std::vector<std::string> &exclusions,
    const std::string &source_label) {
    auto &snap = loaded.snapshot;

    // Filter snapshot using current exclusion list and currently enumerated devices.
    // Note: `m_display_name` is only populated for active displays in libdisplaydevice, so
    // using it here would incorrectly treat inactive-but-connected monitors as missing.
    // For loading/restore, we only require a matching device id (display_name is not required).
    std::set<std::string> valid_devices_norm;
    std::set<std::string> virtual_devices_norm;
    std::vector<std::string> filtered_out_excluded;
    std::vector<std::string> enumerated_devices;
    std::set<std::string> exclusions_norm;
    for (auto id : exclusions) {
      exclusions_norm.insert(normalize_device_id(std::move(id)));
    }

    for (const auto &d : devices) {
      auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
      if (id.empty()) {
        continue;
      }
      enumerated_devices.push_back(id);
      auto norm = normalize_device_id(id);
      if (is_virtual_display_device(d)) {
        if (is_active_display_device(d)) {
          virtual_devices_norm.insert(std::move(norm));
        }
        continue;
      }
      if (!exclusions_norm.empty() && exclusions_norm.count(norm)) {
        filtered_out_excluded.push_back(id);
        continue;
      }
      valid_devices_norm.insert(std::move(norm));
    }

    if (!virtual_devices_norm.empty()) {
      std::vector<std::string> snapshot_devices;
      for (const auto &grp : snap.m_topology) {
        snapshot_devices.insert(snapshot_devices.end(), grp.begin(), grp.end());
      }
      for (const auto &device_id : snapshot_devices) {
        if (virtual_devices_norm.count(normalize_device_id(device_id))) {
          BOOST_LOG(warning) << "Snapshot load rejected: snapshot contains active virtual display device "
                             << device_id << " for path=" << source_label;
          return std::nullopt;
        }
      }
    }

    if (valid_devices_norm.empty()) {
      BOOST_LOG(warning) << "Snapshot load rejected: no valid devices available for path=" << source_label;
      BOOST_LOG(debug) << "Snapshot load rejected details: enumerated_devices=[" << join_ids(enumerated_devices)
                       << "], exclusions=[" << join_ids(exclusions_norm) << "]";
      return std::nullopt;
    }

    auto is_allowed = [&](const std::string &device_id) {
      const auto norm = normalize_device_id(device_id);
      if (!valid_devices_norm.count(norm)) {
        return false;
      }
      return exclusions_norm.empty() || !exclusions_norm.count(norm);
    };

    ActiveTopology filtered_topology;
    for (const auto &grp : snap.m_topology) {
      std::vector<std::string> filtered_grp;
      for (const auto &device_id : grp) {
        if (is_allowed(device_id)) {
          filtered_grp.push_back(device_id);
        } else if (!exclusions_norm.empty() && exclusions_norm.count(normalize_device_id(device_id))) {
          filtered_out_excluded.push_back(device_id);
        }
      }
      if (!filtered_grp.empty()) {
        filtered_topology.push_back(std::move(filtered_grp));
      }
    }

    if (filtered_topology.empty()) {
      BOOST_LOG(warning) << "Snapshot load rejected: all devices filtered for path=" << source_label;
      std::vector<std::string> snapshot_devices;
      for (const auto &grp : snap.m_topology) {
        snapshot_devices.insert(snapshot_devices.end(), grp.begin(), grp.end());
      }
      std::sort(snapshot_devices.begin(), snapshot_devices.end());
      snapshot_devices.erase(std::unique(snapshot_devices.begin(), snapshot_devices.end()), snapshot_devices.end());
      BOOST_LOG(debug) << "Snapshot load rejected details: snapshot_devices=[" << join_ids(snapshot_devices)
                       << "], present_devices=[" << join_ids(valid_devices_norm)
                       << "], exclusions=[" << join_ids(exclusions_norm) << "]";
      return std::nullopt;
    }

    snap.m_topology = std::move(filtered_topology);

    for (auto it = snap.m_modes.begin(); it != snap.m_modes.end();) {
      if (!is_allowed(it->first)) {
        it = snap.m_modes.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = snap.m_hdr_states.begin(); it != snap.m_hdr_states.end();) {
      if (!is_allowed(it->first)) {
        it = snap.m_hdr_states.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = snap.m_origins.begin(); it != snap.m_origins.end();) {
      if (!is_allowed(it->first)) {
        it = snap.m_origins.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = loaded.layout_rotations.begin(); it != loaded.layout_rotations.end();) {
      if (!is_allowed(it->first)) {
        it = loaded.layout_rotations.erase(it);
      } else {
        ++it;
      }
    }
    if (loaded.layout_rotations.empty()) {
      loaded.has_layout_data = false;
    }
    if (!snap.m_primary_device.empty() && !is_allowed(snap.m_primary_device)) {
      snap.m_primary_device.clear();
    }

    if (!filtered_out_excluded.empty()) {
      std::sort(filtered_out_excluded.begin(), filtered_out_excluded.end());
      filtered_out_excluded.erase(std::unique(filtered_out_excluded.begin(), filtered_out_excluded.end()), filtered_out_excluded.end());
      BOOST_LOG(info) << "Snapshot load: excluded devices filtered from " << source_label << ": [" << join_ids(filtered_out_excluded) << "]";
    }

    return loaded;
  }

  bool write_text_atomically(const std::string &text, const std::filesystem::path &path) {
    if (path.empty()) {
      return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    auto temp_path = path;
    temp_path += L".tmp";

    {
      FILE *f = _wfopen(temp_path.wstring().c_str(), L"wb");
      if (!f) {
        return false;
      }
      auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
      const auto written = fwrite(text.data(), 1, text.size(), f);
      if (written != text.size()) {
        guard.reset();
        std::error_code ec_rm_tmp;
        std::filesystem::remove(temp_path, ec_rm_tmp);
        return false;
      }
    }

    std::error_code ec_exist;
    const bool target_exists = std::filesystem::exists(path, ec_exist) && !ec_exist;
    if (!target_exists) {
      std::error_code ec_move;
      std::filesystem::rename(temp_path, path, ec_move);
      if (!ec_move) {
        return true;
      }
    }

    std::error_code ec_copy;
    std::filesystem::copy_file(temp_path, path, std::filesystem::copy_options::overwrite_existing, ec_copy);
    if (ec_copy) {
      return false;
    }

    std::error_code ec_rm_tmp;
    std::filesystem::remove(temp_path, ec_rm_tmp);
    return true;
  }

  std::optional<std::string> read_file_text(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return std::nullopt;
    }
    FILE *f = _wfopen(path.wstring().c_str(), L"rb");
    if (!f) {
      return std::nullopt;
    }
    auto guard = std::unique_ptr<FILE, int (*)(FILE *)>(f, fclose);
    std::string data;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), f)) {
      data.append(buf, n);
    }
    return data;
  }
}  // namespace display_helper::v2::codec

#endif  // _WIN32

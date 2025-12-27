#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace display_helper::v2 {
  FileSnapshotStorage::FileSnapshotStorage(SnapshotPaths paths)
    : paths_(std::move(paths)) {}

  std::optional<Snapshot> FileSnapshotStorage::load(SnapshotTier tier) {
    const auto path = path_for(tier);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }

    auto json = nlohmann::json::parse(file, nullptr, false);
    if (!json.is_object()) {
      return std::nullopt;
    }

    if (!json.contains("topology") || !json.contains("modes") || !json.contains("hdr")) {
      return std::nullopt;
    }

    Snapshot snapshot;

    const auto &topology = json["topology"];
    if (!topology.is_array()) {
      return std::nullopt;
    }
    for (const auto &group : topology) {
      if (!group.is_array()) {
        return std::nullopt;
      }
      std::vector<std::string> ids;
      for (const auto &id : group) {
        if (!id.is_string()) {
          continue;
        }
        ids.push_back(id.get<std::string>());
      }
      if (!ids.empty()) {
        snapshot.m_topology.push_back(std::move(ids));
      }
    }

    const auto &modes = json["modes"];
    if (!modes.is_object()) {
      return std::nullopt;
    }
    for (const auto &[key, value] : modes.items()) {
      if (!value.is_object()) {
        continue;
      }
      display_device::DisplayMode mode;
      mode.m_resolution.m_width = value.value("w", 0u);
      mode.m_resolution.m_height = value.value("h", 0u);
      mode.m_refresh_rate.m_numerator = value.value("num", 0u);
      mode.m_refresh_rate.m_denominator = value.value("den", 0u);
      snapshot.m_modes[key] = mode;
    }

    const auto &hdr = json["hdr"];
    if (!hdr.is_object()) {
      return std::nullopt;
    }
    for (const auto &[key, value] : hdr.items()) {
      if (value.is_null()) {
        snapshot.m_hdr_states[key] = std::nullopt;
      } else if (value.is_string()) {
        const auto state = value.get<std::string>();
        if (state == "on") {
          snapshot.m_hdr_states[key] = display_device::HdrState::Enabled;
        } else if (state == "off") {
          snapshot.m_hdr_states[key] = display_device::HdrState::Disabled;
        } else {
          snapshot.m_hdr_states[key] = std::nullopt;
        }
      }
    }

    if (json.contains("primary") && json["primary"].is_string()) {
      snapshot.m_primary_device = json["primary"].get<std::string>();
    }

    return snapshot;
  }

  bool FileSnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot) {
    const auto path = path_for(tier);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    nlohmann::json json;
    json["topology"] = nlohmann::json::array();
    for (const auto &group : snapshot.m_topology) {
      nlohmann::json group_json = nlohmann::json::array();
      for (const auto &id : group) {
        group_json.push_back(id);
      }
      json["topology"].push_back(std::move(group_json));
    }

    json["modes"] = nlohmann::json::object();
    for (const auto &[id, mode] : snapshot.m_modes) {
      json["modes"][id] = {
        {"w", mode.m_resolution.m_width},
        {"h", mode.m_resolution.m_height},
        {"num", mode.m_refresh_rate.m_numerator},
        {"den", mode.m_refresh_rate.m_denominator},
      };
    }

    json["hdr"] = nlohmann::json::object();
    for (const auto &[id, state] : snapshot.m_hdr_states) {
      if (!state.has_value()) {
        json["hdr"][id] = nullptr;
      } else {
        json["hdr"][id] = (*state == display_device::HdrState::Enabled) ? "on" : "off";
      }
    }

    json["primary"] = snapshot.m_primary_device;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }

    file << json.dump(2);
    return file.good();
  }

  bool FileSnapshotStorage::remove(SnapshotTier tier) {
    const auto path = path_for(tier);
    std::error_code ec;
    return std::filesystem::remove(path, ec);
  }

  std::vector<std::string> FileSnapshotStorage::missing_devices(
    const Snapshot &snapshot,
    const std::set<std::string> &available) {
    std::set<std::string> devices;
    for (const auto &group : snapshot.m_topology) {
      for (const auto &id : group) {
        if (!id.empty()) {
          devices.insert(id);
        }
      }
    }
    if (devices.empty()) {
      for (const auto &entry : snapshot.m_modes) {
        devices.insert(entry.first);
      }
    }

    std::vector<std::string> missing;
    for (const auto &id : devices) {
      if (!available.contains(id)) {
        missing.push_back(id);
      }
    }

    return missing;
  }

  std::filesystem::path FileSnapshotStorage::path_for(SnapshotTier tier) const {
    switch (tier) {
      case SnapshotTier::Current:
        return paths_.current;
      case SnapshotTier::Previous:
        return paths_.previous;
      case SnapshotTier::Golden:
        return paths_.golden;
    }
    return paths_.current;
  }

  std::optional<Snapshot> InMemorySnapshotStorage::load(SnapshotTier tier) {
    auto it = snapshots_.find(tier);
    if (it == snapshots_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool InMemorySnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot) {
    snapshots_[tier] = snapshot;
    return true;
  }

  bool InMemorySnapshotStorage::remove(SnapshotTier tier) {
    return snapshots_.erase(tier) > 0;
  }

  std::vector<std::string> InMemorySnapshotStorage::missing_devices(
    const Snapshot &snapshot,
    const std::set<std::string> &available) {
    std::set<std::string> devices;
    for (const auto &group : snapshot.m_topology) {
      for (const auto &id : group) {
        if (!id.empty()) {
          devices.insert(id);
        }
      }
    }
    if (devices.empty()) {
      for (const auto &entry : snapshot.m_modes) {
        devices.insert(entry.first);
      }
    }

    std::vector<std::string> missing;
    for (const auto &id : devices) {
      if (!available.contains(id)) {
        missing.push_back(id);
      }
    }
    return missing;
  }

  SnapshotService::SnapshotService(IDisplaySettings &display)
    : display_(display) {}

  Snapshot SnapshotService::capture() const {
    return display_.capture_snapshot();
  }

  ApplyStatus SnapshotService::apply(const Snapshot &snapshot, const CancellationToken &token) const {
    if (token.is_cancelled()) {
      return ApplyStatus::Fatal;
    }
    if (!display_.validate_topology(snapshot.m_topology)) {
      return ApplyStatus::InvalidRequest;
    }
    if (!display_.apply_snapshot(snapshot)) {
      return ApplyStatus::Retryable;
    }
    if (token.is_cancelled()) {
      return ApplyStatus::Fatal;
    }
    return ApplyStatus::Ok;
  }

  bool SnapshotService::validate(const Snapshot &snapshot) const {
    return display_.validate_topology(snapshot.m_topology);
  }

  bool SnapshotService::matches_current(const Snapshot &snapshot) const {
    return display_.snapshot_matches_current(snapshot);
  }

  SnapshotPersistence::SnapshotPersistence(ISnapshotStorage &storage)
    : storage_(storage) {}

  void SnapshotPersistence::set_prefer_golden_first(bool prefer) {
    prefer_golden_first_ = prefer;
  }

  std::vector<SnapshotTier> SnapshotPersistence::recovery_order() const {
    if (prefer_golden_first_) {
      return {SnapshotTier::Golden, SnapshotTier::Current, SnapshotTier::Previous};
    }
    return {SnapshotTier::Current, SnapshotTier::Previous, SnapshotTier::Golden};
  }

  bool SnapshotPersistence::save(
    SnapshotTier tier,
    Snapshot snapshot,
    const std::set<std::string> &blacklist) {
    if (!filter_snapshot_devices(snapshot, blacklist)) {
      return false;
    }
    return storage_.save(tier, snapshot);
  }

  std::optional<Snapshot> SnapshotPersistence::load(
    SnapshotTier tier,
    const std::set<std::string> &available) {
    auto snapshot = storage_.load(tier);
    if (!snapshot) {
      return std::nullopt;
    }
    const auto missing = storage_.missing_devices(*snapshot, available);
    if (!missing.empty()) {
      return std::nullopt;
    }
    return snapshot;
  }

  bool SnapshotPersistence::rotate_current_to_previous() {
    auto snapshot = storage_.load(SnapshotTier::Current);
    if (!snapshot) {
      return false;
    }
    return storage_.save(SnapshotTier::Previous, *snapshot);
  }

  bool SnapshotPersistence::remove(SnapshotTier tier) {
    return storage_.remove(tier);
  }

  bool SnapshotPersistence::filter_snapshot_devices(
    Snapshot &snapshot,
    const std::set<std::string> &blacklist) {
    if (blacklist.empty()) {
      return true;
    }

    auto is_allowed = [&](const std::string &device_id) {
      return !blacklist.contains(device_id);
    };

    ActiveTopology filtered_topology;
    for (const auto &group : snapshot.m_topology) {
      std::vector<std::string> filtered_group;
      for (const auto &device_id : group) {
        if (is_allowed(device_id)) {
          filtered_group.push_back(device_id);
        }
      }
      if (!filtered_group.empty()) {
        filtered_topology.push_back(std::move(filtered_group));
      }
    }

    snapshot.m_topology = std::move(filtered_topology);

    for (auto it = snapshot.m_modes.begin(); it != snapshot.m_modes.end();) {
      if (!is_allowed(it->first)) {
        it = snapshot.m_modes.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = snapshot.m_hdr_states.begin(); it != snapshot.m_hdr_states.end();) {
      if (!is_allowed(it->first)) {
        it = snapshot.m_hdr_states.erase(it);
      } else {
        ++it;
      }
    }

    if (!snapshot.m_primary_device.empty() && !is_allowed(snapshot.m_primary_device)) {
      snapshot.m_primary_device.clear();
    }

    if (snapshot.m_topology.empty() && snapshot.m_modes.empty()) {
      return false;
    }

    return true;
  }
}  // namespace display_helper::v2

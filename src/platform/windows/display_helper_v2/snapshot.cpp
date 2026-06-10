#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "src/logging.h"

namespace display_helper::v2 {
  namespace {
    const char *tier_to_string(SnapshotTier tier) {
      switch (tier) {
        case SnapshotTier::Current:
          return "Current";
        case SnapshotTier::Previous:
          return "Previous";
        case SnapshotTier::Golden:
          return "Golden";
        default:
          return "Unknown";
      }
    }

    std::string format_string_vector(const std::vector<std::string> &vec) {
      std::ostringstream oss;
      oss << "[";
      bool first = true;
      for (const auto &s : vec) {
        if (!first) {
          oss << ", ";
        }
        oss << "\"" << s << "\"";
        first = false;
      }
      oss << "]";
      return oss.str();
    }
  }  // namespace
  FileSnapshotStorage::FileSnapshotStorage(SnapshotPaths paths)
    : paths_(std::move(paths)) {}

  std::optional<Snapshot> FileSnapshotStorage::load(SnapshotTier tier) {
    auto loaded = load_with_metadata(tier);
    if (!loaded) {
      return std::nullopt;
    }
    return std::move(loaded->snapshot);
  }

  std::optional<codec::ParsedSnapshot> FileSnapshotStorage::load_with_metadata(SnapshotTier tier) {
    const auto path = path_for(tier);
    const auto text = codec::read_file_text(path);
    if (!text) {
      return std::nullopt;
    }

    auto loaded = codec::parse_snapshot_text(*text);
    if (loaded.snapshot.m_topology.empty() && loaded.snapshot.m_modes.empty()) {
      return std::nullopt;
    }
    return loaded;
  }

  bool FileSnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot) {
    return save(tier, snapshot, {});
  }

  bool FileSnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot, const codec::layout_rotation_map_t &layout_rotations) {
    const auto path = path_for(tier);
    const auto text = codec::serialize_snapshot(snapshot, layout_rotations);
    return codec::write_text_atomically(text, path);
  }

  bool FileSnapshotStorage::remove(SnapshotTier tier) {
    const auto path = path_for(tier);
    std::error_code ec;
    return std::filesystem::remove(path, ec);
  }

  bool FileSnapshotStorage::exists(SnapshotTier tier) {
    std::error_code ec;
    return std::filesystem::exists(path_for(tier), ec) && !ec;
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

  EnumeratedDeviceList SnapshotService::enumerate() const {
    return display_.enumerate(display_device::DeviceEnumerationDetail::Minimal);
  }

  codec::layout_rotation_map_t SnapshotService::capture_layouts(const std::set<std::string> &device_ids) const {
    return display_.capture_layout_rotations(device_ids);
  }

  bool SnapshotService::topology_is_valid(const ActiveTopology &topology) const {
    return display_.topology_is_valid(topology);
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
    BOOST_LOG(debug) << "Snapshot: attempting to load tier '" << tier_to_string(tier) << "'";

    auto snapshot = storage_.load(tier);
    if (!snapshot) {
      BOOST_LOG(debug) << "Snapshot: tier '" << tier_to_string(tier) << "' not found or invalid";
      return std::nullopt;
    }

    const auto missing = storage_.missing_devices(*snapshot, available);
    if (!missing.empty()) {
      BOOST_LOG(info) << "Snapshot: tier '" << tier_to_string(tier)
                      << "' rejected - missing devices: " << format_string_vector(missing);
      return std::nullopt;
    }

    BOOST_LOG(debug) << "Snapshot: tier '" << tier_to_string(tier) << "' loaded successfully";
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

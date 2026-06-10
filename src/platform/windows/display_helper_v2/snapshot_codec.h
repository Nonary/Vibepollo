#pragma once

#include "src/platform/windows/display_helper_v2/types.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/**
 * @file snapshot_codec.h
 * @brief Pure snapshot serialization/parsing/filtering logic shared with the legacy
 *        display helper. The on-disk JSON format and all filtering rules are ported
 *        verbatim from tools/display_settings_helper.cpp so snapshots written by either
 *        engine round-trip through the other.
 */
namespace display_helper::v2::codec {
  using layout_rotation_map_t = std::map<std::string, int>;

  inline constexpr int kSnapshotLayoutVersionLatest = 2;

  struct ParsedSnapshot {
    Snapshot snapshot;
    int snapshot_version {1};
    bool has_layout_data {false};
    layout_rotation_map_t layout_rotations;
  };

  // --- string helpers ---
  std::string ascii_lower(std::string s);
  bool contains_ci(const std::string &haystack, const std::string &needle);
  bool equals_ci(const std::string &lhs, const std::string &rhs);
  std::string normalize_device_id(std::string id);
  std::optional<int> normalize_rotation_degrees(int degrees);

  // --- topology/snapshot helpers ---
  std::vector<std::string> flatten_topology_device_ids(const ActiveTopology &topology);
  std::set<std::string> snapshot_device_set(const Snapshot &s);
  std::set<std::string> topology_device_set(const ActiveTopology &topology);

  // Windows enumerates topology groups in an arbitrary, session-dependent order;
  // only the set of groups (and their members) is meaningful, mirroring
  // WinDisplayDevice::isTopologyTheSame.
  ActiveTopology canonical_topology(ActiveTopology topology);

  // Strict comparator: require full structural equality; allow Unknown==Unknown for HDR.
  // Topology is compared order-insensitively so a restore isn't treated as failed
  // (and endlessly re-applied) just because the OS enumerates paths in a new order.
  bool equal_snapshots_strict(const Snapshot &a, const Snapshot &b);

  // Stable textual representation for change detection/logging (canonical topology order).
  std::string signature(const Snapshot &snap);

  // --- device classification ---
  bool is_virtual_display_device(const display_device::EnumeratedDevice &device);
  bool is_active_display_device(const display_device::EnumeratedDevice &device);

  // --- serialization (legacy on-disk JSON format) ---
  std::string serialize_snapshot(const Snapshot &snap, const layout_rotation_map_t &layout_rotations);
  ParsedSnapshot parse_snapshot_text(const std::string &data);
  bool snapshot_text_has_restore_payload(const std::string &data);

  // --- filtering rules ---

  /**
   * @brief Save-side filtering: reject snapshots that contain active virtual displays,
   *        drop devices without a display_name (not safe restore targets), apply the
   *        exclusion list, and reject when nothing restorable remains.
   * @param exclusions Normalized (normalize_device_id) exclusion list.
   * @returns Filtered snapshot, or nullopt with reject_reason set.
   */
  std::optional<Snapshot> filter_snapshot_for_save(
    Snapshot snap,
    const EnumeratedDeviceList &devices,
    const std::vector<std::string> &exclusions,
    std::string &reject_reason);

  /**
   * @brief Load-side filtering: reject snapshots referencing active virtual displays,
   *        filter exclusions and devices not currently present, and reject when the
   *        remaining topology is empty. Mirrors the legacy loader exactly: only a
   *        matching device id is required (display_name is not, since it is only
   *        populated for active displays).
   * @param exclusions Exclusion list (normalized internally).
   * @param source_label Used for log messages (e.g. the file path).
   */
  std::optional<ParsedSnapshot> filter_loaded_snapshot(
    ParsedSnapshot loaded,
    const EnumeratedDeviceList &devices,
    const std::vector<std::string> &exclusions,
    const std::string &source_label);

  // --- file IO ---
  bool write_text_atomically(const std::string &text, const std::filesystem::path &path);
  std::optional<std::string> read_file_text(const std::filesystem::path &path);
}  // namespace display_helper::v2::codec

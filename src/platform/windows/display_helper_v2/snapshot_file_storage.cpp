#include "src/platform/windows/display_helper_v2/snapshot_file_storage.h"

namespace display_helper::v2 {
  namespace {
    SnapshotStorageKeys keys_from(const SnapshotPaths &paths) {
      return {paths.current.string(), paths.previous.string(), paths.golden.string()};
    }
  }

  FileSnapshotStorage::FileSnapshotStorage(SnapshotPaths paths)
    : core_(keys_from(paths), files_) {}

  std::optional<Snapshot> FileSnapshotStorage::load(SnapshotTier tier) { return core_.load(tier); }
  std::optional<codec::ParsedSnapshot> FileSnapshotStorage::load_with_metadata(SnapshotTier tier) { return core_.load_with_metadata(tier); }
  bool FileSnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot) { return core_.save(tier, snapshot); }
  bool FileSnapshotStorage::save(SnapshotTier tier, const Snapshot &snapshot, const codec::layout_rotation_map_t &layouts) { return core_.save(tier, snapshot, layouts); }
  bool FileSnapshotStorage::remove(SnapshotTier tier) { return core_.remove(tier); }
  bool FileSnapshotStorage::exists(SnapshotTier tier) { return core_.exists(tier); }
  std::vector<std::string> FileSnapshotStorage::missing_devices(const Snapshot &snapshot, const std::set<std::string> &available) { return core_.missing_devices(snapshot, available); }
}  // namespace display_helper::v2

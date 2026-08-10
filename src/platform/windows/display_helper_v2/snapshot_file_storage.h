#pragma once

#include "src/platform/windows/display_helper_v2/file_text_storage.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"

#include <filesystem>

namespace display_helper::v2 {
  struct SnapshotPaths {
    std::filesystem::path current;
    std::filesystem::path previous;
    std::filesystem::path golden;
  };

  /// Runtime filesystem adapter retaining the legacy public storage shape.
  class FileSnapshotStorage final : public ISnapshotStorage {
  public:
    explicit FileSnapshotStorage(SnapshotPaths paths);

    std::optional<Snapshot> load(SnapshotTier tier) override;
    std::optional<codec::ParsedSnapshot> load_with_metadata(SnapshotTier tier) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot, const codec::layout_rotation_map_t &layout_rotations) override;
    bool remove(SnapshotTier tier) override;
    bool exists(SnapshotTier tier) override;
    std::vector<std::string> missing_devices(const Snapshot &snapshot, const std::set<std::string> &available) override;

  private:
    AtomicFileTextStorage files_;
    TextSnapshotStorage core_;
  };
}  // namespace display_helper::v2

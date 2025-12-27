#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"

#include <filesystem>
#include <map>
#include <set>
#include <vector>

namespace display_helper::v2 {
  struct SnapshotPaths {
    std::filesystem::path current;
    std::filesystem::path previous;
    std::filesystem::path golden;
  };

  class FileSnapshotStorage final : public ISnapshotStorage {
  public:
    explicit FileSnapshotStorage(SnapshotPaths paths);

    std::optional<Snapshot> load(SnapshotTier tier) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot) override;
    bool remove(SnapshotTier tier) override;
    std::vector<std::string> missing_devices(
      const Snapshot &snapshot,
      const std::set<std::string> &available) override;

  private:
    SnapshotPaths paths_;

    std::filesystem::path path_for(SnapshotTier tier) const;
  };

  class InMemorySnapshotStorage : public ISnapshotStorage {
  public:
    std::optional<Snapshot> load(SnapshotTier tier) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot) override;
    bool remove(SnapshotTier tier) override;
    std::vector<std::string> missing_devices(
      const Snapshot &snapshot,
      const std::set<std::string> &available) override;

  private:
    std::map<SnapshotTier, Snapshot> snapshots_;
  };

  class SnapshotService {
  public:
    explicit SnapshotService(IDisplaySettings &display);

    Snapshot capture() const;
    ApplyStatus apply(const Snapshot &snapshot, const CancellationToken &token) const;
    bool validate(const Snapshot &snapshot) const;
    bool matches_current(const Snapshot &snapshot) const;

  private:
    IDisplaySettings &display_;
  };

  class SnapshotPersistence {
  public:
    explicit SnapshotPersistence(ISnapshotStorage &storage);

    void set_prefer_golden_first(bool prefer);
    std::vector<SnapshotTier> recovery_order() const;

    bool save(SnapshotTier tier, Snapshot snapshot, const std::set<std::string> &blacklist);
    std::optional<Snapshot> load(SnapshotTier tier, const std::set<std::string> &available);
    bool rotate_current_to_previous();
    bool remove(SnapshotTier tier);

  private:
    ISnapshotStorage &storage_;
    bool prefer_golden_first_ = false;

    static bool filter_snapshot_devices(Snapshot &snapshot, const std::set<std::string> &blacklist);
  };
}  // namespace display_helper::v2

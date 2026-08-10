#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/text_storage.h"

#include <map>
#include <set>
#include <vector>

namespace display_helper::v2 {
  struct SnapshotStorageKeys {
    std::string current;
    std::string previous;
    std::string golden;
  };

  /// Snapshot serialization/persistence policy.  Text I/O is injected so the
  /// recovery core remains deterministic and portable.
  class TextSnapshotStorage final : public ISnapshotStorage {
  public:
    TextSnapshotStorage(SnapshotStorageKeys keys, ITextStorage &text_storage);

    std::optional<Snapshot> load(SnapshotTier tier) override;
    std::optional<codec::ParsedSnapshot> load_with_metadata(SnapshotTier tier) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot) override;
    bool save(SnapshotTier tier, const Snapshot &snapshot, const codec::layout_rotation_map_t &layout_rotations) override;
    bool remove(SnapshotTier tier) override;
    bool exists(SnapshotTier tier) override;
    std::vector<std::string> missing_devices(
      const Snapshot &snapshot,
      const std::set<std::string> &available) override;

    const std::string &key_for(SnapshotTier tier) const;

  private:
    SnapshotStorageKeys keys_;
    ITextStorage &text_storage_;
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

    EnumeratedDeviceList enumerate() const;
    codec::layout_rotation_map_t capture_layouts(const std::set<std::string> &device_ids) const;
    bool topology_is_valid(const ActiveTopology &topology) const;

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

    ISnapshotStorage &storage() {
      return storage_;
    }

  private:
    ISnapshotStorage &storage_;
    bool prefer_golden_first_ = false;

    static bool filter_snapshot_devices(Snapshot &snapshot, const std::set<std::string> &blacklist);
  };
}  // namespace display_helper::v2

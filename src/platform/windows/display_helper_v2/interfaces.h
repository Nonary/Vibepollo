#pragma once

#include "src/platform/windows/display_helper_v2/snapshot_codec.h"
#include "src/platform/windows/display_helper_v2/types.h"

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace display_helper::v2 {
  class IDisplaySettings {
  public:
    virtual ~IDisplaySettings() = default;

    /// Apply one complete SettingsManager transaction. Ordinary APPLY callers
    /// may set their supplied base topology once immediately before this call;
    /// the backend must still receive the original device-preparation request.
    virtual ApplyStatus apply(const SingleDisplayConfiguration &config) = 0;
    virtual ApplyStatus apply_topology(const ActiveTopology &topology) = 0;
    virtual EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail detail) = 0;
    virtual ActiveTopology capture_topology() = 0;
    /// Validate a topology stored in a restore snapshot. Structurally invalid
    /// snapshots must be rejected; transient OS validation failures should be
    /// retried by the staged restore path rather than discarded permanently.
    virtual bool validate_topology(const ActiveTopology &topology) = 0;

    virtual Snapshot capture_snapshot() = 0;
    virtual bool apply_snapshot(const Snapshot &snapshot) = 0;

    /// Apply snapshot settings after its topology has been activated and
    /// settled. The default preserves compatibility with simple test fakes.
    virtual bool apply_snapshot_settings(const Snapshot &snapshot) {
      return apply_snapshot(snapshot);
    }

    virtual bool snapshot_matches_current(const Snapshot &snapshot) = 0;
    virtual bool configuration_matches(const SingleDisplayConfiguration &config) = 0;
    /// Verify configuration fields using the target scope resolved by the
    /// single Apply preflight. The default keeps small test/alternate
    /// backends compatible while preserving empty-id primary-group semantics
    /// for callers that implement the richer overload.
    virtual bool configuration_matches(
      const SingleDisplayConfiguration &config,
      const ResolvedConfigurationTarget &target) {
      if (target.duplicate_device_ids.empty()) {
        return configuration_matches(config);
      }
      for (const auto &device_id : target.duplicate_device_ids) {
        auto scoped = config;
        scoped.m_device_id = device_id;
        if (!configuration_matches(scoped)) {
          return false;
        }
      }
      return true;
    }
    virtual bool set_display_origin(const std::string &device_id, const display_device::Point &origin) = 0;
    virtual bool is_topology_same(const ActiveTopology &lhs, const ActiveTopology &rhs) = 0;

    // --- Apply preflight and restore capabilities (defaulted so test fakes only override what they assert on) ---

    /// Cheap structural validity check (isTopologyValid).
    virtual bool topology_is_valid(const ActiveTopology &topology) {
      return !topology.empty();
    }

    /// Attempt a display stack recovery between failed topology stages.
    virtual bool recover_display_stack() {
      return false;
    }

    /// Preflight an ordinary APPLY against its supplied staging topology. This
    /// mirrors the legacy helper's cheap SDC_VALIDATE check and must not mutate
    /// or recover the global display stack.
    virtual ApplyPreflightOutcome preflight_apply(
      const SingleDisplayConfiguration &config,
      const std::optional<ActiveTopology> &base_topology) {
      return ApplyPreflightOutcome {
        .status = ApplyStatus::Ok,
        .plan = ApplyTopologyPlan {
          .topology = base_topology.value_or(ActiveTopology {}),
          .activation_target = TopologyActivationTarget {
            .kind = config.m_device_id.empty() ?
                      DeviceTargetKind::DefaultPrimaryGroup :
                      DeviceTargetKind::ExplicitDevice,
            .acceptable_device_ids = config.m_device_id.empty() ?
                                       std::set<std::string> {} :
                                       std::set<std::string> {config.m_device_id},
          },
        },
      };
    }

    /// Clear session-scoped settings state after a confirmed restore.
    virtual bool reset_staged_apply_state() {
      return true;
    }

    /// Capture per-device rotation (degrees) for the given device ids.
    virtual codec::layout_rotation_map_t capture_layout_rotations(const std::set<std::string> &) {
      return {};
    }

    /// Apply per-device rotations (batched, mirrors legacy CDS_NORESET commit).
    virtual bool apply_layout_rotations(const codec::layout_rotation_map_t &) {
      return true;
    }

    /// True when the current rotations match the expected layout map.
    virtual bool current_layout_matches(const codec::layout_rotation_map_t &) {
      return true;
    }

    /// Force a same-value rotation mode-set for non-default rotations. The OS
    /// can report a matching layout while the display driver's pointer
    /// transform is stale after a virtual-display session; only a forced
    /// re-apply reaches the driver. Best-effort: failure must not fail a
    /// confirmed restore.
    virtual bool reassert_layout_rotations(const codec::layout_rotation_map_t &) {
      return true;
    }

    /// Restore a device's refresh rate to num/den.
    virtual bool set_device_refresh_rate(const std::string &, unsigned int, unsigned int) {
      return false;
    }

    /// Restore several physical refresh rates from one captured mode map. The
    /// default preserves alternate backends; Windows overrides it so ordinary
    /// Apply does not perform one QueryDisplayConfig/set cycle per monitor.
    virtual std::set<std::string> set_device_refresh_rates(
      const std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> &overrides) {
      std::set<std::string> applied;
      for (const auto &[device_id, rate] : overrides) {
        if (set_device_refresh_rate(device_id, rate.first, rate.second)) {
          applied.insert(device_id);
        }
      }
      return applied;
    }

    /// Resolution of an active device (used to clamp monitor position overrides).
    virtual std::optional<display_device::Resolution> get_display_resolution(const std::string &) {
      return std::nullopt;
    }

    /// True when the device is currently active and can be repositioned.
    virtual bool can_reposition_device(const std::string &) {
      return true;
    }

    /// Capture active/repositionable state and optional resolution for all
    /// requested monitors in one enumeration. Presence means active; the
    /// optional resolution controls only GDI-bound clamping.
    virtual std::unordered_map<std::string, std::optional<display_device::Resolution>>
    get_repositionable_display_resolutions(const std::set<std::string> &device_ids) {
      std::unordered_map<std::string, std::optional<display_device::Resolution>> result;
      for (const auto &device_id : device_ids) {
        if (can_reposition_device(device_id)) {
          result.emplace(device_id, get_display_resolution(device_id));
        }
      }
      return result;
    }
  };

  class ISnapshotStorage {
  public:
    virtual ~ISnapshotStorage() = default;

    virtual std::optional<Snapshot> load(SnapshotTier tier) = 0;
    virtual bool save(SnapshotTier tier, const Snapshot &snapshot) = 0;
    virtual bool remove(SnapshotTier tier) = 0;
    virtual std::vector<std::string> missing_devices(
      const Snapshot &snapshot,
      const std::set<std::string> &available) = 0;

    /// Load including schema version and display layout (rotation) metadata.
    virtual std::optional<codec::ParsedSnapshot> load_with_metadata(SnapshotTier tier) {
      auto snapshot = load(tier);
      if (!snapshot) {
        return std::nullopt;
      }
      codec::ParsedSnapshot loaded;
      loaded.snapshot = std::move(*snapshot);
      loaded.snapshot_version = codec::kSnapshotLayoutVersionLatest;
      return loaded;
    }

    /// Save including display layout (rotation) metadata.
    virtual bool save(SnapshotTier tier, const Snapshot &snapshot, const codec::layout_rotation_map_t &layout_rotations) {
      (void) layout_rotations;
      return save(tier, snapshot);
    }

    /// True when the tier exists on storage (even if it would fail validation).
    virtual bool exists(SnapshotTier tier) {
      return load(tier).has_value();
    }

    /// Move (not copy) the current session snapshot into the previous slot so the
    /// restore chain keeps one level of history (legacy promote_current_snapshot_to_previous).
    virtual bool promote_current_to_previous() {
      auto current = load_with_metadata(SnapshotTier::Current);
      if (!current) {
        return false;
      }
      if (!save(SnapshotTier::Previous, current->snapshot, current->layout_rotations)) {
        return false;
      }
      (void) remove(SnapshotTier::Current);
      return true;
    }
  };

  class IVirtualDisplayDriver {
  public:
    virtual ~IVirtualDisplayDriver() = default;

    virtual bool disable() = 0;
    virtual bool enable() = 0;
    virtual bool is_available() = 0;
    virtual std::string device_id() = 0;
  };

  class IClock {
  public:
    virtual ~IClock() = default;

    virtual std::chrono::steady_clock::time_point now() = 0;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
  };

  class IScheduledTaskManager {
  public:
    virtual ~IScheduledTaskManager() = default;

    virtual bool create_restore_task(const std::wstring &username) = 0;
    virtual bool delete_restore_task() = 0;
    virtual bool is_task_present() = 0;
  };

  class IPlatformWorkarounds {
  public:
    virtual ~IPlatformWorkarounds() = default;

    virtual void blank_hdr_states(std::chrono::milliseconds delay) = 0;
    virtual void clear_pending_hdr_blank() = 0;
    virtual void refresh_shell() = 0;
  };
}  // namespace display_helper::v2

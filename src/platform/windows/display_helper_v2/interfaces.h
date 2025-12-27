#pragma once

#include "src/platform/windows/display_helper_v2/types.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace display_helper::v2 {
  class IDisplaySettings {
  public:
    virtual ~IDisplaySettings() = default;

    virtual ApplyStatus apply(const SingleDisplayConfiguration &config) = 0;
    virtual ApplyStatus apply_topology(const ActiveTopology &topology) = 0;
    virtual EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail detail) = 0;
    virtual ActiveTopology capture_topology() = 0;
    virtual bool validate_topology(const ActiveTopology &topology) = 0;
    virtual Snapshot capture_snapshot() = 0;
    virtual bool apply_snapshot(const Snapshot &snapshot) = 0;
    virtual bool snapshot_matches_current(const Snapshot &snapshot) = 0;
    virtual bool configuration_matches(const SingleDisplayConfiguration &config) = 0;
    virtual bool set_display_origin(const std::string &device_id, const display_device::Point &origin) = 0;
    virtual std::optional<ActiveTopology> compute_expected_topology(
      const SingleDisplayConfiguration &config,
      const std::optional<ActiveTopology> &base_topology = std::nullopt) = 0;
    virtual bool is_topology_same(const ActiveTopology &lhs, const ActiveTopology &rhs) = 0;
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
    virtual void refresh_shell() = 0;
  };
}  // namespace display_helper::v2

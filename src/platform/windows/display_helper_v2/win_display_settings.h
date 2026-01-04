#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include <display_device/noop_audio_context.h>
#include <display_device/noop_settings_persistence.h>
#include <display_device/windows/settings_manager.h>
#include <display_device/windows/settings_utils.h>
#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_api_utils.h>
#include <display_device/windows/win_display_device.h>

namespace display_helper::v2 {
  class WinDisplaySettings final : public IDisplaySettings {
  public:
    ApplyStatus apply(const SingleDisplayConfiguration &config) override;
    ApplyStatus apply_topology(const ActiveTopology &topology) override;
    EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail detail) override;
    ActiveTopology capture_topology() override;
    bool validate_topology(const ActiveTopology &topology) override;
    Snapshot capture_snapshot() override;
    bool apply_snapshot(const Snapshot &snapshot) override;
    bool snapshot_matches_current(const Snapshot &snapshot) override;
    bool configuration_matches(const SingleDisplayConfiguration &config) override;
    bool set_display_origin(const std::string &device_id, const display_device::Point &origin) override;
    std::optional<ActiveTopology> compute_expected_topology(
      const SingleDisplayConfiguration &config,
      const std::optional<ActiveTopology> &base_topology = std::nullopt) override;
    bool is_topology_same(const ActiveTopology &lhs, const ActiveTopology &rhs) override;

  private:
    enum class InitState : std::uint8_t {
      Uninitialized,
      Ready,
      Failed,
    };

    bool ensure_initialized() const;
    bool validate_topology_with_os(const ActiveTopology &topology) const;

    std::optional<std::string> find_primary_in_set(const std::set<std::string> &ids) const;
    void collect_all_device_ids(std::set<std::string> &out) const;

    static std::optional<double> floating_to_double(const display_device::FloatingPoint &value);
    static bool nearly_equal(double lhs, double rhs);
    ApplyStatus map_apply_result(display_device::SettingsManagerInterface::ApplyResult result) const;

    mutable std::once_flag init_once_;
    mutable std::atomic<InitState> init_state_ {InitState::Uninitialized};
    mutable std::shared_ptr<display_device::WinApiLayer> win_api_;
    mutable std::shared_ptr<display_device::WinDisplayDevice> display_device_;
    mutable std::unique_ptr<display_device::SettingsManager> settings_manager_;
  };
}  // namespace display_helper::v2

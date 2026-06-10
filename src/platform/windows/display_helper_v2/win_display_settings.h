#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

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

    // legacy engine capabilities
    bool topology_is_valid(const ActiveTopology &topology) override;
    bool soft_test(const SingleDisplayConfiguration &config, const std::optional<ActiveTopology> &base_topology) override;
    bool recover_display_stack() override;
    codec::layout_rotation_map_t capture_layout_rotations(const std::set<std::string> &device_ids) override;
    bool apply_layout_rotations(const codec::layout_rotation_map_t &layout_rotations) override;
    bool current_layout_matches(const codec::layout_rotation_map_t &expected) override;
    bool set_device_refresh_rate(const std::string &device_id, unsigned int num, unsigned int den) override;
    std::optional<display_device::Resolution> get_display_resolution(const std::string &device_id) override;
    bool can_reposition_device(const std::string &device_id) override;

    /// Apply a snapshot including layout rotations (legacy restore semantics).
    bool apply_snapshot_with_layouts(const Snapshot &snapshot, const codec::layout_rotation_map_t *layout_rotations);

  private:
    std::unordered_map<std::string, std::wstring> active_display_names_by_device_id(const std::set<std::string> &device_ids = {}) const;
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

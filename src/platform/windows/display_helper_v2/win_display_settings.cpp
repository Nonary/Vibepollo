#include "src/platform/windows/display_helper_v2/win_display_settings.h"

#include "src/logging.h"
#include "src/platform/windows/display_helper_v2/snapshot_codec.h"
#include "src/platform/windows/display_helper_v2/staged_settings.h"
#include "src/platform/windows/display_helper_v2/topology_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>
#include <display_device/windows/persistent_state.h>
#include <display_device/windows/win_api_recovery.h>

#include <windows.h>

namespace display_helper::v2 {
  namespace {
    template <typename MapType>
    std::string format_map_keys(const MapType &map) {
      std::ostringstream oss;
      oss << "[";
      bool first = true;
      for (const auto &[key, _] : map) {
        if (!first) {
          oss << ", ";
        }
        oss << "\"" << key << "\"";
        first = false;
      }
      oss << "]";
      return oss.str();
    }

    std::string format_topology(const ActiveTopology &topology) {
      std::ostringstream oss;
      oss << "[";
      bool first_group = true;
      for (const auto &group : topology) {
        if (!first_group) {
          oss << ", ";
        }
        oss << "[";
        bool first_id = true;
        for (const auto &id : group) {
          if (!first_id) {
            oss << ", ";
          }
          oss << "\"" << id << "\"";
          first_id = false;
        }
        oss << "]";
        first_group = false;
      }
      oss << "]";
      return oss.str();
    }
  }  // namespace
  ApplyStatus WinDisplaySettings::apply(const SingleDisplayConfiguration &config) {
    if (!ensure_initialized()) {
      return ApplyStatus::HelperUnavailable;
    }

    std::lock_guard lock(settings_mutex_);
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    std::optional<display_device::SingleDisplayConfigState> previous_state;
    bool persistent_state_rebased = false;
    const auto restore_rebased_persistent_state = [&] {
      // A staged topology retry may reuse this SettingsManager instance. Its
      // original primary/mode/HDR baseline must survive every failure path,
      // including non-std exceptions raised by the display stack.
      if (persistent_state_rebased && settings_state_ && !settings_state_->persistState(previous_state)) {
        BOOST_LOG(error) << "Display helper v2: failed to preserve persistent state after settings apply failure.";
      }
    };
    try {
      const auto current_topology = display_device_->getCurrentTopology();
      if (!display_device_->isTopologyValid(current_topology)) {
        return ApplyStatus::VerificationFailed;
      }

      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      const auto current_initial = StagedSettingsState::rebased_initial(
        session_initial_state_,
        current_topology,
        devices);
      if (!current_initial || !settings_state_) {
        return ApplyStatus::VerificationFailed;
      }

      // Retain v1's original primary/mode/HDR values across consecutive APPLYs,
      // but rebase the topology fields onto the result already accepted by the
      // staged transition. This prevents VerifyOnly from restoring an old layout.
      previous_state = settings_state_->getState();
      const auto rebased = StagedSettingsState::rebase(
        previous_state,
        *current_initial,
        current_topology);
      if (!settings_state_->persistState(rebased)) {
        return ApplyStatus::Retryable;
      }
      persistent_state_rebased = true;

      // EnsurePrimary must remain visible to SettingsManager so it can capture
      // and later restore the original primary. Other preparation modes become
      // VerifyOnly because topology activation is already complete.
      const auto settings_config = StagedSettingsState::settings_configuration(config);
      const auto result = map_apply_result(settings_manager_->applySettings(settings_config));
      if (result != ApplyStatus::Ok) {
        restore_rebased_persistent_state();
      }
      return result;
    } catch (const std::exception &e) {
      // The staged topology can be retried by the state machine, but retaining
      // a rebased SettingsManager state after a failed settings mutation would
      // lose the user's original primary/mode/HDR baseline on the next APPLY.
      restore_rebased_persistent_state();
      BOOST_LOG(error) << "Display helper v2: staged settings apply failed: " << e.what();
      return ApplyStatus::Fatal;
    } catch (...) {
      restore_rebased_persistent_state();
      BOOST_LOG(error) << "Display helper v2: staged settings apply failed.";
      return ApplyStatus::Fatal;
    }
  }

  ApplyStatus WinDisplaySettings::apply_topology(const ActiveTopology &topology) {
    if (!ensure_initialized()) {
      return ApplyStatus::HelperUnavailable;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      if (display_device_->setTopology(topology)) {
        return ApplyStatus::Ok;
      }
      return ApplyStatus::VerificationFailed;
    } catch (...) {
      return ApplyStatus::Fatal;
    }
  }

  EnumeratedDeviceList WinDisplaySettings::enumerate(display_device::DeviceEnumerationDetail detail) {
    if (!ensure_initialized()) {
      return {};
    }

    // TopologyTransition owns the retry cadence; keep each poll short and
    // side-effect free instead of letting a query trigger nested recovery.
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_device_->enumAvailableDevices(detail);
    } catch (...) {
      return {};
    }
  }

  ActiveTopology WinDisplaySettings::capture_topology() {
    if (!ensure_initialized()) {
      return {};
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_device_->getCurrentTopology();
    } catch (...) {
      return {};
    }
  }

  bool WinDisplaySettings::validate_topology(const ActiveTopology &topology) {
    if (!ensure_initialized()) {
      return false;
    }

    try {
      if (!display_device_->isTopologyValid(topology)) {
        return false;
      }

      // SDC_VALIDATE is useful diagnostics, but it can return access denied
      // while Windows is transitioning the desktop after a virtual display is
      // removed. The snapshot is still structurally valid and must reach the
      // real apply path, which can recover and retry the display stack.
      if (!validate_topology_with_os(topology)) {
        BOOST_LOG(warning) << "Display helper v2: OS topology probe failed; preserving structurally valid restore snapshot for retry.";
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::validate_topology_for_apply(const ActiveTopology &topology) {
    if (!ensure_initialized()) {
      return false;
    }
    try {
      return display_device_->isTopologyValid(topology) && validate_topology_with_os(topology);
    } catch (...) {
      return false;
    }
  }

  Snapshot WinDisplaySettings::capture_snapshot() {
    Snapshot snapshot;
    if (!ensure_initialized()) {
      return snapshot;
    }

    try {
      snapshot.m_topology = display_device_->getCurrentTopology();

      std::set<std::string> device_ids;
      for (const auto &group : snapshot.m_topology) {
        device_ids.insert(group.begin(), group.end());
      }
      if (device_ids.empty()) {
        collect_all_device_ids(device_ids);
      }

      snapshot.m_modes = display_device_->getCurrentDisplayModes(device_ids);
      snapshot.m_hdr_states = display_device_->getCurrentHdrStates(device_ids);

      if (auto primary = find_primary_in_set(device_ids)) {
        snapshot.m_primary_device = *primary;
      }

      // Origins (monitor positions)
      for (const auto &d : display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (!id.empty() && d.m_info && device_ids.count(id)) {
          snapshot.m_origins[id] = d.m_info->m_origin_point;
        }
      }
    } catch (...) {
    }

    return snapshot;
  }

  bool WinDisplaySettings::apply_snapshot(const Snapshot &snapshot) {
    return apply_snapshot_with_layouts(snapshot, nullptr);
  }

  bool WinDisplaySettings::apply_snapshot_settings(const Snapshot &snapshot) {
    if (!ensure_initialized()) {
      BOOST_LOG(error) << "apply_snapshot_settings: display device not initialized";
      return false;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // Preserve v1's restore order: topology is staged by TopologyTransition,
      // then modes and HDR are restored before primary/origin changes. Moving
      // primary first can make Windows reject a saved mode on a waking or
      // duplicate display, leaving recovery half-applied.
      if (!snapshot.m_modes.empty() && !display_device_->setDisplayModes(snapshot.m_modes)) {
        BOOST_LOG(warning) << "apply_snapshot_settings: failed to restore display modes";
        return false;
      }
      if (!snapshot.m_hdr_states.empty() && !display_device_->setHdrStates(snapshot.m_hdr_states)) {
        BOOST_LOG(warning) << "apply_snapshot_settings: failed to restore HDR states";
        return false;
      }
      if (!snapshot.m_primary_device.empty() && !display_device_->setAsPrimary(snapshot.m_primary_device)) {
        BOOST_LOG(warning) << "apply_snapshot_settings: failed to restore primary display";
        return false;
      }
      for (const auto &[device_id, point] : snapshot.m_origins) {
        if (!display_device_->setDisplayOrigin(device_id, point)) {
          BOOST_LOG(warning) << "apply_snapshot_settings: failed to restore origin for " << device_id;
          return false;
        }
      }
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "apply_snapshot_settings: exception - " << e.what();
      return false;
    } catch (...) {
      BOOST_LOG(error) << "apply_snapshot_settings: unknown exception";
      return false;
    }
  }

  bool WinDisplaySettings::apply_snapshot_with_layouts(const Snapshot &snapshot, const codec::layout_rotation_map_t *layout_rotations) {
    if (!ensure_initialized()) {
      BOOST_LOG(error) << "apply_snapshot: display device not initialized";
      return false;
    }

    BOOST_LOG(info) << "apply_snapshot: applying snapshot with:"
                    << "\n  topology: " << format_topology(snapshot.m_topology)
                    << "\n  modes for devices: " << format_map_keys(snapshot.m_modes)
                    << "\n  HDR states for devices: " << format_map_keys(snapshot.m_hdr_states)
                    << "\n  primary device: " << (snapshot.m_primary_device.empty() ? "(none)" : snapshot.m_primary_device);

    try {
      // Do not suppress failures here. RecoveryOperation treats a failed apply
      // as retryable; reporting success after Windows rejected every operation
      // prevented that recovery path from observing the real failure.
      if (apply_topology(snapshot.m_topology) != ApplyStatus::Ok) {
        BOOST_LOG(warning) << "apply_snapshot: failed to restore topology";
        return false;
      }
      if (!apply_snapshot_settings(snapshot)) {
        return false;
      }
      if (layout_rotations && !layout_rotations->empty()) {
        if (!apply_layout_rotations(*layout_rotations)) {
          BOOST_LOG(warning) << "apply_snapshot: failed to restore display rotations";
          return false;
        }
      }
      BOOST_LOG(info) << "apply_snapshot: completed";
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "apply_snapshot: exception - " << e.what();
      return false;
    } catch (...) {
      BOOST_LOG(error) << "apply_snapshot: unknown exception";
      return false;
    }
  }

  bool WinDisplaySettings::snapshot_matches_current(const Snapshot &snapshot) {
    if (!ensure_initialized()) {
      return false;
    }

    try {
      // Order-insensitive comparison: Windows enumerates topology groups in an
      // arbitrary order, and treating that as a mismatch caused endless restore
      // re-applies that scrambled MPO planes (fd98755b).
      return codec::equal_snapshots_strict(capture_snapshot(), snapshot);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::configuration_matches(const SingleDisplayConfiguration &config) {
    if (config.m_device_id.empty()) {
      return false;
    }

    return configuration_matches(
      config,
      ResolvedConfigurationTarget {
        .kind = DeviceTargetKind::ExplicitDevice,
        .representative_device_id = config.m_device_id,
        .duplicate_device_ids = {config.m_device_id},
      });
  }

  bool WinDisplaySettings::configuration_matches(
    const SingleDisplayConfiguration &config,
    const ResolvedConfigurationTarget &target) {
    if (!ensure_initialized()) {
      return false;
    }

    std::set<std::string> mode_device_ids = target.duplicate_device_ids;
    if (mode_device_ids.empty() && !target.representative_device_id.empty()) {
      mode_device_ids.insert(target.representative_device_id);
    }
    if (mode_device_ids.empty() && !config.m_device_id.empty()) {
      mode_device_ids.insert(config.m_device_id);
    }
    if (mode_device_ids.empty()) {
      return false;
    }

    std::set<std::string> refresh_hdr_device_ids;
    if (target.kind == DeviceTargetKind::DefaultPrimaryGroup) {
      refresh_hdr_device_ids = mode_device_ids;
    } else if (!target.representative_device_id.empty()) {
      refresh_hdr_device_ids.insert(target.representative_device_id);
    } else if (!config.m_device_id.empty()) {
      refresh_hdr_device_ids.insert(config.m_device_id);
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      if (config.m_resolution || config.m_refresh_rate) {
        auto modes = display_device_->getCurrentDisplayModes(mode_device_ids);
        for (const auto &device_id : mode_device_ids) {
          const auto it = modes.find(device_id);
          if (it == modes.end()) {
            return false;
          }
          const auto &mode = it->second;
          if (config.m_resolution &&
              (mode.m_resolution.m_width != config.m_resolution->m_width ||
               mode.m_resolution.m_height != config.m_resolution->m_height)) {
            return false;
          }
          if (config.m_refresh_rate && refresh_hdr_device_ids.count(device_id)) {
            if (!refresh_rates_match(*config.m_refresh_rate, mode.m_refresh_rate)) {
              return false;
            }
          }
        }
      }

      if (config.m_hdr_state) {
        auto hdr_states = display_device_->getCurrentHdrStates(refresh_hdr_device_ids);
        for (const auto &device_id : refresh_hdr_device_ids) {
          const auto it = hdr_states.find(device_id);
          if (it == hdr_states.end() || !it->second) {
            if (*config.m_hdr_state == display_device::HdrState::Disabled) {
              continue;
            }
            return false;
          }
          if (*it->second != *config.m_hdr_state) {
            return false;
          }
        }
      }

      return true;
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::set_display_origin(const std::string &device_id, const display_device::Point &origin) {
    if (!ensure_initialized()) {
      return false;
    }

    // Treat monitor reposition as part of APPLY semantics (no recovery).
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_device_->setDisplayOrigin(device_id, origin);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::topology_is_valid(const ActiveTopology &topology) {
    if (!ensure_initialized()) {
      return false;
    }
    try {
      return display_device_->isTopologyValid(topology);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::recover_display_stack() {
    if (!ensure_initialized()) {
      return false;
    }
    try {
      win_api_->recoverDisplayStack();
      return true;
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::prepare_staged_apply(const ActiveTopology &current_topology) {
    if (!ensure_initialized()) {
      return false;
    }

    std::lock_guard lock(settings_mutex_);
    if (session_initial_state_) {
      return true;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      if (!display_device_->isTopologyValid(current_topology)) {
        return false;
      }
      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      auto initial = StagedSettingsState::topology_base(
        std::nullopt,
        current_topology,
        devices);
      if (!initial) {
        return false;
      }
      session_initial_state_ = std::move(*initial);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::reset_staged_apply_state() {
    if (!ensure_initialized()) {
      return false;
    }

    std::lock_guard lock(settings_mutex_);
    if (!settings_manager_->resetPersistence()) {
      return false;
    }
    session_initial_state_.reset();
    return true;
  }

  bool WinDisplaySettings::is_primary_device(const std::string &device_id) {
    if (device_id.empty() || !ensure_initialized()) {
      return false;
    }
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_device_->isPrimary(device_id);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::set_device_refresh_rate(const std::string &device_id, unsigned int num, unsigned int den) {
    if (device_id.empty() || !ensure_initialized()) {
      return false;
    }
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      std::set<std::string> device_set {device_id};
      auto current_modes = display_device_->getCurrentDisplayModes(device_set);
      if (current_modes.count(device_id)) {
        const auto &current = current_modes[device_id].m_refresh_rate;
        if (current.m_denominator != 0 &&
            static_cast<std::uint64_t>(current.m_numerator) * den ==
              static_cast<std::uint64_t>(num) * current.m_denominator) {
          return true;
        }
        current_modes[device_id].m_refresh_rate = display_device::Rational {num, den};
        return display_device_->setDisplayModes(current_modes);
      }
    } catch (...) {
    }
    return false;
  }

  std::optional<display_device::Resolution> WinDisplaySettings::get_display_resolution(const std::string &device_id) {
    if (device_id.empty() || !ensure_initialized()) {
      return std::nullopt;
    }
    try {
      const auto normalized = codec::normalize_device_id(device_id);
      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      for (const auto &device : devices) {
        if (device.m_device_id.empty() || codec::normalize_device_id(device.m_device_id) != normalized) {
          continue;
        }
        if (device.m_info) {
          return device.m_info->m_resolution;
        }
        return std::nullopt;
      }
    } catch (...) {
    }
    return std::nullopt;
  }

  bool WinDisplaySettings::can_reposition_device(const std::string &device_id) {
    if (device_id.empty() || !ensure_initialized()) {
      return false;
    }
    try {
      const auto normalized = codec::normalize_device_id(device_id);
      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      for (const auto &device : devices) {
        if (device.m_device_id.empty()) {
          continue;
        }
        if (codec::normalize_device_id(device.m_device_id) != normalized) {
          continue;
        }
        // Only attempt reposition for currently active displays.
        return static_cast<bool>(device.m_info);
      }
    } catch (...) {
    }
    return false;
  }

  std::optional<ActiveTopology> WinDisplaySettings::compute_expected_topology(
    const SingleDisplayConfiguration &config,
    const std::optional<ActiveTopology> &base_topology) {
    auto plan = compute_apply_topology_plan(config, base_topology);
    if (!plan) {
      return std::nullopt;
    }
    return std::move(plan->topology);
  }

  std::optional<ApplyTopologyPlan> WinDisplaySettings::compute_apply_topology_plan(
    const SingleDisplayConfiguration &config,
    const std::optional<ActiveTopology> &base_topology) {
    if (!ensure_initialized()) {
      return std::nullopt;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    std::lock_guard lock(settings_mutex_);
    try {
      auto topology_before = base_topology.value_or(display_device_->getCurrentTopology());
      if (!display_device_->isTopologyValid(topology_before)) {
        return std::nullopt;
      }

      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      auto initial = StagedSettingsState::topology_base(
        session_initial_state_,
        topology_before,
        devices);
      if (!initial) {
        return std::nullopt;
      }

      const auto [new_topology, device_to_configure, additional_devices] = topology::compute_new_topology_and_metadata(
        config.m_device_prep,
        config.m_device_id,
        *initial
      );
      TopologyActivationTarget activation_target;
      activation_target.kind = config.m_device_id.empty() ?
                                 DeviceTargetKind::DefaultPrimaryGroup :
                                 DeviceTargetKind::ExplicitDevice;
      if (!device_to_configure.empty()) {
        activation_target.acceptable_device_ids.insert(device_to_configure);
      }
      if (activation_target.kind == DeviceTargetKind::DefaultPrimaryGroup) {
        activation_target.acceptable_device_ids.insert(additional_devices.begin(), additional_devices.end());
      }
      return ApplyTopologyPlan {
        .topology = new_topology,
        .activation_target = std::move(activation_target),
      };
    } catch (...) {
      return std::nullopt;
    }
  }

  bool WinDisplaySettings::is_topology_same(const ActiveTopology &lhs, const ActiveTopology &rhs) {
    if (!ensure_initialized()) {
      return false;
    }

    try {
      return display_device_->isTopologyTheSame(lhs, rhs);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::ensure_initialized() const {
    auto state = init_state_.load(std::memory_order_acquire);
    if (state == InitState::Ready) {
      return true;
    }
    if (state == InitState::Failed) {
      return false;
    }

    std::call_once(init_once_, [this]() {
      try {
        auto win_api = std::make_shared<display_device::WinApiLayer>();
        auto display = std::make_shared<display_device::WinDisplayDevice>(win_api);
        auto persistent_state = std::make_unique<display_device::PersistentState>(
          std::make_shared<display_device::NoopSettingsPersistence>());
        auto *persistent_state_ptr = persistent_state.get();
        auto settings = std::make_unique<display_device::SettingsManager>(
          display,
          std::make_shared<display_device::NoopAudioContext>(),
          std::move(persistent_state),
          display_device::WinWorkarounds {}
        );

        win_api_ = std::move(win_api);
        display_device_ = std::move(display);
        settings_manager_ = std::move(settings);
        settings_state_ = persistent_state_ptr;
        init_state_.store(InitState::Ready, std::memory_order_release);
      } catch (...) {
        BOOST_LOG(error) << "Display helper v2: failed to initialize display settings.";
        init_state_.store(InitState::Failed, std::memory_order_release);
      }
    });

    return init_state_.load(std::memory_order_acquire) == InitState::Ready;
  }

  bool WinDisplaySettings::validate_topology_with_os(const ActiveTopology &topology) const {
    if (!display_device_->isTopologyValid(topology)) {
      return false;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    const auto original_data = win_api_->queryDisplayConfig(display_device::QueryType::All);
    if (!original_data) {
      return false;
    }

    const auto path_data = display_device::win_utils::collectSourceDataForMatchingPaths(*win_api_, original_data->m_paths);
    if (path_data.empty()) {
      return false;
    }

    auto paths = display_device::win_utils::makePathsForNewTopology(topology, path_data, original_data->m_paths);
    if (paths.empty()) {
      return false;
    }

    UINT32 flags = SDC_VALIDATE | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE;
    LONG result = win_api_->setDisplayConfig(paths, {}, flags);
    if (result == ERROR_GEN_FAILURE) {
      flags = SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE;
      result = win_api_->setDisplayConfig(paths, {}, flags);
    }
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Display helper v2: topology validation failed: " << result;
      return false;
    }
    return true;
  }

  std::optional<std::string> WinDisplaySettings::find_primary_in_set(const std::set<std::string> &ids) const {
    if (!ensure_initialized()) {
      return std::nullopt;
    }
    for (const auto &id : ids) {
      if (display_device_->isPrimary(id)) {
        return id;
      }
    }
    return std::nullopt;
  }

  void WinDisplaySettings::collect_all_device_ids(std::set<std::string> &out) const {
    auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
    for (const auto &device : devices) {
      const auto id = device.m_device_id.empty() ? device.m_display_name : device.m_device_id;
      if (!id.empty()) {
        out.insert(id);
      }
    }
  }

  std::optional<double> WinDisplaySettings::floating_to_double(const display_device::FloatingPoint &value) {
    if (std::holds_alternative<double>(value)) {
      return std::get<double>(value);
    }
    const auto &rat = std::get<display_device::Rational>(value);
    if (rat.m_denominator == 0) {
      return std::nullopt;
    }
    return static_cast<double>(rat.m_numerator) / static_cast<double>(rat.m_denominator);
  }

  bool WinDisplaySettings::refresh_rates_match(
    const display_device::FloatingPoint &desired,
    const display_device::FloatingPoint &actual
  ) {
    const auto *desired_rational = std::get_if<display_device::Rational>(&desired);
    const auto *actual_rational = std::get_if<display_device::Rational>(&actual);
    if (desired_rational && actual_rational) {
      return display_device::win_utils::fuzzyCompareRefreshRates(*desired_rational, *actual_rational);
    }

    // Production display configurations and active Windows modes are rational,
    // but retain equivalent semantics for any legacy double-valued caller.
    constexpr double kRefreshRateToleranceHz = 0.9;
    const auto desired_value = floating_to_double(desired);
    const auto actual_value = floating_to_double(actual);
    return desired_value &&
           actual_value &&
           std::abs(*desired_value - *actual_value) <= kRefreshRateToleranceHz;
  }

  namespace {
    std::optional<int> dmdo_to_degrees(DWORD orientation) {
      switch (orientation) {
        case DMDO_DEFAULT:
          return 0;
        case DMDO_90:
          return 90;
        case DMDO_180:
          return 180;
        case DMDO_270:
          return 270;
        default:
          return std::nullopt;
      }
    }

    std::optional<DWORD> degrees_to_dmdo(int degrees) {
      auto normalized = codec::normalize_rotation_degrees(degrees);
      if (!normalized) {
        return std::nullopt;
      }
      switch (*normalized) {
        case 0:
          return DMDO_DEFAULT;
        case 90:
          return DMDO_90;
        case 180:
          return DMDO_180;
        case 270:
          return DMDO_270;
        default:
          return std::nullopt;
      }
    }

    /**
     * @brief Dynamically allocate and populate a full DEVMODEW (including dmDriverExtra)
     *        for the given display. This avoids truncating driver-specific data that some
     *        GPU drivers attach beyond the standard DEVMODEW structure.
     */
    std::vector<uint8_t> alloc_full_devmode(const std::wstring &display_name) {
      DEVMODEW probe {};
      probe.dmSize = sizeof(DEVMODEW);
      probe.dmDriverExtra = 0;
      if (!EnumDisplaySettingsExW(display_name.c_str(), ENUM_CURRENT_SETTINGS, &probe, 0)) {
        return {};
      }

      const size_t total = static_cast<size_t>(probe.dmSize) + probe.dmDriverExtra;
      std::vector<uint8_t> buffer(total, 0);
      auto *mode = reinterpret_cast<DEVMODEW *>(buffer.data());
      mode->dmSize = probe.dmSize;
      mode->dmDriverExtra = probe.dmDriverExtra;

      if (!EnumDisplaySettingsExW(display_name.c_str(), ENUM_CURRENT_SETTINGS, mode, 0)) {
        return {};
      }
      return buffer;
    }

    std::optional<int> read_display_rotation_degrees(const std::wstring &display_name) {
      if (display_name.empty()) {
        return std::nullopt;
      }
      auto buf = alloc_full_devmode(display_name);
      if (buf.empty()) {
        return std::nullopt;
      }
      const auto *mode = reinterpret_cast<const DEVMODEW *>(buf.data());
      return dmdo_to_degrees(mode->dmDisplayOrientation);
    }

    struct PreparedRotation {
      std::wstring display_name;
      std::vector<uint8_t> devmode_buffer;  ///< Heap buffer holding the full DEVMODEW + dmDriverExtra
      bool already_correct = false;
    };

    std::optional<PreparedRotation> prepare_display_rotation(const std::wstring &display_name, int degrees) {
      if (display_name.empty()) {
        return std::nullopt;
      }
      auto target = degrees_to_dmdo(degrees);
      if (!target) {
        return std::nullopt;
      }

      auto buf = alloc_full_devmode(display_name);
      if (buf.empty()) {
        return std::nullopt;
      }
      auto *mode = reinterpret_cast<DEVMODEW *>(buf.data());

      if (mode->dmDisplayOrientation == *target) {
        return PreparedRotation {display_name, {}, true};
      }

      const bool swap_axes = ((mode->dmDisplayOrientation + *target) % 2) == 1;
      mode->dmFields = DM_DISPLAYORIENTATION | DM_POSITION;
      mode->dmDisplayOrientation = *target;
      if (swap_axes) {
        std::swap(mode->dmPelsWidth, mode->dmPelsHeight);
        mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
      }

      return PreparedRotation {display_name, std::move(buf), false};
    }
  }  // namespace

  std::unordered_map<std::string, std::wstring> WinDisplaySettings::active_display_names_by_device_id(const std::set<std::string> &device_ids) const {
    std::unordered_map<std::string, std::wstring> out;
    if (!ensure_initialized()) {
      return out;
    }
    const bool filter = !device_ids.empty();
    try {
      for (const auto &d : display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
        const auto id = d.m_device_id.empty() ? d.m_display_name : d.m_device_id;
        if (id.empty()) {
          continue;
        }
        if (filter && !device_ids.contains(id)) {
          continue;
        }

        std::string display_name = d.m_display_name;
        if (display_name.empty()) {
          try {
            display_name = display_device_->getDisplayName(id);
          } catch (...) {
          }
        }
        if (display_name.empty()) {
          continue;
        }

        const std::wstring display_name_w(display_name.begin(), display_name.end());
        out.emplace(id, display_name_w);
        const auto id_lower = codec::ascii_lower(id);
        if (id_lower != id) {
          out.emplace(id_lower, display_name_w);
        }
      }
    } catch (...) {
    }
    return out;
  }

  codec::layout_rotation_map_t WinDisplaySettings::capture_layout_rotations(const std::set<std::string> &device_ids) {
    codec::layout_rotation_map_t out;
    if (!ensure_initialized()) {
      return out;
    }

    auto names = active_display_names_by_device_id(device_ids);
    for (const auto &[device_id, display_name] : names) {
      if (auto rotation = read_display_rotation_degrees(display_name)) {
        out.emplace(device_id, *rotation);
      }
    }
    return out;
  }

  bool WinDisplaySettings::apply_layout_rotations(const codec::layout_rotation_map_t &layout_rotations) {
    if (layout_rotations.empty()) {
      return true;
    }
    if (!ensure_initialized()) {
      return false;
    }

    auto names = active_display_names_by_device_id();

    // --- Phase 1: Prepare all rotation changes without applying them ---
    std::vector<PreparedRotation> pending;
    bool all_ok = true;

    for (const auto &[device_id, rotation] : layout_rotations) {
      auto it = names.find(device_id);
      if (it == names.end()) {
        BOOST_LOG(warning) << "Layout restore: device missing while applying rotation: " << device_id;
        all_ok = false;
        continue;
      }

      auto prepared = prepare_display_rotation(it->second, rotation);
      if (!prepared) {
        BOOST_LOG(warning) << "Layout restore: failed to prepare rotation for " << device_id
                           << " (" << rotation << " degrees)";
        all_ok = false;
        continue;
      }

      if (!prepared->already_correct) {
        pending.push_back(std::move(*prepared));
      }
    }

    if (pending.empty()) {
      return all_ok;  // All displays already at correct rotation
    }

    // Fast path: single display doesn't need batching
    if (pending.size() == 1) {
      auto *request = reinterpret_cast<DEVMODEW *>(pending[0].devmode_buffer.data());
      LONG result = ChangeDisplaySettingsExW(pending[0].display_name.c_str(), request, nullptr, CDS_UPDATEREGISTRY, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        result = ChangeDisplaySettingsExW(pending[0].display_name.c_str(), request, nullptr, 0, nullptr);
      }
      if (result != DISP_CHANGE_SUCCESSFUL) {
        BOOST_LOG(warning) << "Layout restore: ChangeDisplaySettingsEx failed for display "
                           << std::string(pending[0].display_name.begin(), pending[0].display_name.end())
                           << " (error=" << result << ")";
        all_ok = false;
      }
      return all_ok;
    }

    // --- Phase 2: Batch all changes to registry with CDS_NORESET ---
    // Each call writes to the registry but does NOT trigger a mode change.
    // This prevents the OS from validating intermediate topological states.
    for (auto &prep : pending) {
      auto *request = reinterpret_cast<DEVMODEW *>(prep.devmode_buffer.data());
      LONG result = ChangeDisplaySettingsExW(
        prep.display_name.c_str(), request, nullptr,
        CDS_UPDATEREGISTRY | CDS_NORESET, nullptr
      );
      if (result != DISP_CHANGE_SUCCESSFUL) {
        BOOST_LOG(warning) << "Layout restore: CDS_NORESET batch failed for display "
                           << std::string(prep.display_name.begin(), prep.display_name.end())
                           << " (error=" << result << ")";
        all_ok = false;
      }
    }

    // --- Phase 3: Atomic commit — apply all batched registry changes at once ---
    // A single null-call triggers one WM_DISPLAYCHANGE and one topology validation.
    LONG commit_result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (commit_result != DISP_CHANGE_SUCCESSFUL) {
      BOOST_LOG(warning) << "Layout restore: atomic commit of batched rotations failed (error=" << commit_result << ")";
      all_ok = false;
    }

    return all_ok;
  }

  bool WinDisplaySettings::current_layout_matches(const codec::layout_rotation_map_t &expected) {
    if (expected.empty()) {
      return true;
    }
    if (!ensure_initialized()) {
      return false;
    }

    auto names = active_display_names_by_device_id();
    for (const auto &[device_id, expected_rotation] : expected) {
      auto it = names.find(device_id);
      if (it == names.end()) {
        return false;
      }
      auto current_rotation = read_display_rotation_degrees(it->second);
      if (!current_rotation || *current_rotation != expected_rotation) {
        return false;
      }
    }
    return true;
  }

  ApplyStatus WinDisplaySettings::map_apply_result(display_device::SettingsManagerInterface::ApplyResult result) const {
    using enum display_device::SettingsManagerInterface::ApplyResult;
    switch (result) {
      case Ok:
        return ApplyStatus::Ok;
      case ApiTemporarilyUnavailable:
        return ApplyStatus::Retryable;
      case DevicePrepFailed:
      case PrimaryDevicePrepFailed:
      case DisplayModePrepFailed:
        return ApplyStatus::VerificationFailed;
      case HdrStatePrepFailed:
        return ApplyStatus::HdrStateFailed;
      case PersistenceSaveFailed:
        return ApplyStatus::Retryable;
      default:
        return ApplyStatus::Fatal;
    }
  }
}  // namespace display_helper::v2

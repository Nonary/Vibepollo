#include "src/platform/windows/display_helper_v2/win_display_settings.h"

#include "src/logging.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <display_device/windows/persistent_state.h>

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

    const auto result = settings_manager_->applySettings(config);
    return map_apply_result(result);
  }

  ApplyStatus WinDisplaySettings::apply_topology(const ActiveTopology &topology) {
    if (!ensure_initialized()) {
      return ApplyStatus::HelperUnavailable;
    }

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

    return validate_topology_with_os(topology);
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
    } catch (...) {
    }

    return snapshot;
  }

  bool WinDisplaySettings::apply_snapshot(const Snapshot &snapshot) {
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
      BOOST_LOG(debug) << "apply_snapshot: setting topology";
      (void) display_device_->setTopology(snapshot.m_topology);

      BOOST_LOG(debug) << "apply_snapshot: setting display modes for " << snapshot.m_modes.size() << " device(s)";
      (void) display_device_->setDisplayModesTemporary(snapshot.m_modes);

      BOOST_LOG(debug) << "apply_snapshot: setting HDR states for " << snapshot.m_hdr_states.size() << " device(s)";
      (void) display_device_->setHdrStates(snapshot.m_hdr_states);

      if (!snapshot.m_primary_device.empty()) {
        BOOST_LOG(debug) << "apply_snapshot: setting primary device to " << snapshot.m_primary_device;
        (void) display_device_->setAsPrimary(snapshot.m_primary_device);
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
      return capture_snapshot() == snapshot;
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::configuration_matches(const SingleDisplayConfiguration &config) {
    if (!ensure_initialized()) {
      return false;
    }

    if (config.m_device_id.empty()) {
      return false;
    }

    try {
      const std::set<std::string> device_ids {config.m_device_id};

      if (config.m_resolution || config.m_refresh_rate) {
        auto modes = display_device_->getCurrentDisplayModes(device_ids);
        auto it = modes.find(config.m_device_id);
        if (it == modes.end()) {
          return false;
        }

        const auto &mode = it->second;
        if (config.m_resolution) {
          if (mode.m_resolution.m_width != config.m_resolution->m_width ||
              mode.m_resolution.m_height != config.m_resolution->m_height) {
            return false;
          }
        }

        if (config.m_refresh_rate) {
          auto desired = floating_to_double(*config.m_refresh_rate);
          auto actual = floating_to_double(mode.m_refresh_rate);
          if (!desired || !actual || !nearly_equal(*desired, *actual)) {
            return false;
          }
        }
      }

      if (config.m_hdr_state) {
        auto hdr_states = display_device_->getCurrentHdrStates(device_ids);
        auto it = hdr_states.find(config.m_device_id);
        if (it == hdr_states.end() || !it->second || *it->second != *config.m_hdr_state) {
          return false;
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

    try {
      return display_device_->setDisplayOrigin(device_id, origin);
    } catch (...) {
      return false;
    }
  }

  std::optional<ActiveTopology> WinDisplaySettings::compute_expected_topology(
    const SingleDisplayConfiguration &config,
    const std::optional<ActiveTopology> &base_topology) {
    if (!ensure_initialized()) {
      return std::nullopt;
    }

    try {
      auto topology_before = base_topology.value_or(display_device_->getCurrentTopology());
      if (!display_device_->isTopologyValid(topology_before)) {
        return std::nullopt;
      }

      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      auto initial = display_device::win_utils::computeInitialState(std::nullopt, topology_before, devices);
      if (!initial) {
        return std::nullopt;
      }

      const auto [new_topology, device_to_configure, additional_devices] = display_device::win_utils::computeNewTopologyAndMetadata(
        config.m_device_prep,
        config.m_device_id,
        *initial
      );
      return new_topology;
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
        auto settings = std::make_unique<display_device::SettingsManager>(
          display,
          std::make_shared<display_device::NoopAudioContext>(),
          std::make_unique<display_device::PersistentState>(std::make_shared<display_device::NoopSettingsPersistence>()),
          display_device::WinWorkarounds {}
        );

        win_api_ = std::move(win_api);
        display_device_ = std::move(display);
        settings_manager_ = std::move(settings);
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

  bool WinDisplaySettings::nearly_equal(double lhs, double rhs) {
    const double diff = std::abs(lhs - rhs);
    const double scale = std::max({1.0, std::abs(lhs), std::abs(rhs)});
    return diff <= scale * 1e-4;
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
      case HdrStatePrepFailed:
        return ApplyStatus::VerificationFailed;
      case PersistenceSaveFailed:
        return ApplyStatus::Retryable;
      default:
        return ApplyStatus::Fatal;
    }
  }
}  // namespace display_helper::v2

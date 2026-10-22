#include "src/platform/windows/display_helper_borrow_native.h"
#include "src/platform/windows/display_helper_v2/enumeration_status.h"
#include "src/platform/windows/display_helper_v2/win_display_settings.h"

#include "src/logging.h"
#include "src/utility.h"
#include "src/platform/windows/display_helper_v2/snapshot_codec.h"
#include "src/platform/windows/display_helper_v2/topology_policy.h"
#include "src/platform/windows/display_snapshot_restore.h"


#include <thread>
#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <display_device/windows/persistent_state.h>
#include <display_device/windows/win_api_recovery.h>

#include <windows.h>

namespace display_helper::v2 {
  namespace {
    std::optional<std::u16string> monitor_path_to_utf16(const std::string &monitor_path) {
      if (monitor_path.empty() || monitor_path.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
      }

      const auto output_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        monitor_path.data(),
        static_cast<int>(monitor_path.size()),
        nullptr,
        0
      );
      if (output_size <= 0) {
        return std::nullopt;
      }

      std::wstring wide_path(static_cast<std::size_t>(output_size), L'\0');
      if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            monitor_path.data(),
            static_cast<int>(monitor_path.size()),
            wide_path.data(),
            output_size
          ) != output_size) {
        return std::nullopt;
      }

      std::u16string utf16_path;
      utf16_path.reserve(wide_path.size());
      for (const auto code_unit : wide_path) {
        utf16_path.push_back(static_cast<char16_t>(code_unit));
      }
      return utf16_path;
    }

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
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return ApplyStatus::HelperUnavailable;
    if (!ensure_initialized()) {
      return ApplyStatus::HelperUnavailable;
    }

    std::lock_guard lock(settings_mutex_);
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      const auto status = map_apply_result(settings_manager_->applySettings(config));
      if (status == ApplyStatus::Ok && !config.m_device_id.empty()) {
        // Keep only state verified from the active desktop after a successful
        // full transaction. The recovery lane can use this if the request did
        // not pin a resolution, refresh rate, or HDR state explicitly.
        try {
          const auto active = win_api_->queryDisplayConfig(display_device::QueryType::Active);
          if (active) {
            for (const auto &path : active->m_paths) {
              if (!boost::iequals(win_api_->getDeviceId(path), config.m_device_id)) {
                continue;
              }
              if (const auto index = display_device::win_utils::getSourceIndex(path, active->m_modes)) {
                verified_sources_[config.m_device_id] = active->m_modes.at(*index).sourceMode;
                verified_refreshes_[config.m_device_id] = path.targetInfo.refreshRate;
                const auto states = display_device_->getCurrentHdrStates({config.m_device_id});
                if (const auto hdr = states.find(config.m_device_id); hdr != states.end() && hdr->second) {
                  verified_hdr_states_[config.m_device_id] = *hdr->second;
                }
              }
              break;
            }
          }
        } catch (...) {
          // This cache improves an otherwise underspecified live repair; its
          // failure must not turn a completed APPLY into an error.
        }
      }
      return status;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Display helper v2: display settings apply failed: " << e.what();
      return ApplyStatus::Fatal;
    } catch (...) {
      BOOST_LOG(error) << "Display helper v2: display settings apply failed.";
      return ApplyStatus::Fatal;
    }
  }

  ApplyStatus WinDisplaySettings::reactivate_virtual_target(
    const SingleDisplayConfiguration &config,
    const std::function<bool()> &cancelled
  ) {
    if (!ensure_initialized() || config.m_device_id.empty() || cancelled()) {
      return ApplyStatus::Retryable;
    }
    std::lock_guard lock(settings_mutex_);
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);

    auto desired_hdr = config.m_hdr_state;
    if (!desired_hdr) {
      if (const auto saved = verified_hdr_states_.find(config.m_device_id); saved != verified_hdr_states_.end()) {
        desired_hdr = saved->second;
      }
    }

    const auto query = win_api_->queryDisplayConfig(display_device::QueryType::All);
    if (!query || cancelled()) {
      return ApplyStatus::Retryable;
    }
    const auto same_luid = [](const LUID &lhs, const LUID &rhs) {
      return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
    };

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::optional<DISPLAYCONFIG_PATH_INFO> candidate;
    LONG right = 0;
    for (const auto &path : query->m_paths) {
      const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      if (boost::iequals(win_api_->getDeviceId(path), config.m_device_id)) {
        // The fullscreen transition may have settled while the repair was
        // queued; an active target is already healthy.
        if (active) {
          return ApplyStatus::Ok;
        }
        const bool source_busy = std::any_of(query->m_paths.begin(), query->m_paths.end(), [&](const auto &other) {
          return (other.flags & DISPLAYCONFIG_PATH_ACTIVE) &&
                 same_luid(path.sourceInfo.adapterId, other.sourceInfo.adapterId) &&
                 path.sourceInfo.id == other.sourceInfo.id;
        });
        if (path.targetInfo.targetAvailable && !source_busy && !candidate) {
          candidate = path;
        }
      }
      if (active) {
        paths.push_back(path);
        if (const auto index = display_device::win_utils::getSourceIndex(path, query->m_modes)) {
          const auto &mode = query->m_modes.at(*index).sourceMode;
          const auto edge = static_cast<std::int64_t>(mode.position.x) + mode.width;
          right = static_cast<LONG>(std::min<std::int64_t>(LONG_MAX, std::max<std::int64_t>(right, edge)));
        }
      }
    }
    if (!candidate) {
      return ApplyStatus::Retryable;
    }

    auto resolution = config.m_resolution;
    if (!resolution) {
      const auto saved = verified_sources_.find(config.m_device_id);
      if (saved == verified_sources_.end()) {
        return ApplyStatus::Retryable;
      }
      resolution = display_device::Resolution {saved->second.width, saved->second.height};
    }

    DISPLAYCONFIG_RATIONAL rate {};
    if (config.m_refresh_rate) {
      if (const auto rational = std::get_if<display_device::Rational>(&*config.m_refresh_rate)) {
        rate = {rational->m_numerator, rational->m_denominator};
      } else {
        const auto number = floating_to_double(*config.m_refresh_rate);
        if (!number || !std::isfinite(*number) || *number <= 0 || *number > UINT32_MAX / 1000.0) {
          return ApplyStatus::InvalidRequest;
        }
        rate = {static_cast<UINT32>(std::llround(*number * 1000.0)), 1000};
      }
    } else if (const auto saved = verified_refreshes_.find(config.m_device_id); saved != verified_refreshes_.end()) {
      rate = saved->second;
    }
    if (rate.Numerator == 0 || rate.Denominator == 0) {
      return ApplyStatus::Retryable;
    }

    auto modes = query->m_modes;
    auto target = *candidate;
    target.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    DISPLAYCONFIG_MODE_INFO source {};
    source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    source.id = target.sourceInfo.id;
    source.adapterId = target.sourceInfo.adapterId;
    source.sourceMode.width = resolution->m_width;
    source.sourceMode.height = resolution->m_height;
    source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    source.sourceMode.position = {right, 0};
    if (const auto saved = verified_sources_.find(config.m_device_id); saved != verified_sources_.end()) {
      source.sourceMode.position = saved->second.position;
    }
    if (const auto index = display_device::win_utils::getSourceIndex(target, query->m_modes)) {
      source.sourceMode.position = query->m_modes.at(*index).sourceMode.position;
    }
    const auto overlaps_peer = [&](const POINTL &origin) {
      return std::any_of(paths.begin(), paths.end(), [&](const auto &path) {
        const auto index = display_device::win_utils::getSourceIndex(path, query->m_modes);
        if (!index) {
          return true;
        }
        const auto &peer = query->m_modes.at(*index).sourceMode;
        return static_cast<std::int64_t>(origin.x) < static_cast<std::int64_t>(peer.position.x) + peer.width &&
               static_cast<std::int64_t>(origin.x) + source.sourceMode.width > peer.position.x &&
               static_cast<std::int64_t>(origin.y) < static_cast<std::int64_t>(peer.position.y) + peer.height &&
               static_cast<std::int64_t>(origin.y) + source.sourceMode.height > peer.position.y;
      });
    };
    // Windows can promote a peer to (0,0) after losing a virtual primary.
    // Preserve that live peer instead of introducing overlapping sources.
    if (overlaps_peer(source.sourceMode.position)) {
      source.sourceMode.position = {right, 0};
    }

    target.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    target.targetInfo.refreshRate = rate;
    const bool virtual_modes = (target.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
    const auto source_index = static_cast<UINT32>(modes.size());
    modes.push_back(source);
    if (virtual_modes) {
      target.sourceInfo.sourceModeInfoIdx = source_index;
      target.sourceInfo.cloneGroupId = 0xffffu;
      target.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
      target.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    } else {
      target.sourceInfo.modeInfoIdx = source_index;
      target.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    paths.push_back(target);

    // Start with Windows-selected target timing. If the driver refuses that
    // timing, retry with the same explicit descriptor used by exact target
    // activation. In either case only the current peer paths plus this target
    // are supplied; no topology reset or saved layout is replayed.
    const auto refresh_millihz = (static_cast<std::uint64_t>(rate.Numerator) * 1000u + rate.Denominator / 2u) /
                                rate.Denominator;
    if (resolution->m_width == 0 || resolution->m_height == 0 ||
        resolution->m_width > static_cast<unsigned int>(LONG_MAX) ||
        resolution->m_height > static_cast<unsigned int>(LONG_MAX) || refresh_millihz == 0 ||
        refresh_millihz > UINT32_MAX) {
      return ApplyStatus::InvalidRequest;
    }
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO target_signal {};
    const auto pixel_count = static_cast<std::uint64_t>(resolution->m_width) * resolution->m_height;
    const auto pixel_rate = pixel_count > UINT64_MAX / refresh_millihz ?
                              UINT64_MAX :
                              (pixel_count * refresh_millihz) / 1000u;
    target_signal.pixelRate = pixel_rate;
    target_signal.hSyncFreq.Numerator = static_cast<UINT32>(std::min<std::uint64_t>(
      UINT32_MAX,
      refresh_millihz * resolution->m_height
    ));
    target_signal.hSyncFreq.Denominator = 1000u;
    target_signal.vSyncFreq.Numerator = static_cast<UINT32>(refresh_millihz);
    target_signal.vSyncFreq.Denominator = 1000u;
    target_signal.activeSize.cx = static_cast<LONG>(resolution->m_width);
    target_signal.activeSize.cy = static_cast<LONG>(resolution->m_height);
    target_signal.totalSize = target_signal.activeSize;
    target_signal.AdditionalSignalInfo.videoStandard = 255;
    target_signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
    target_signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    std::vector<DISPLAYCONFIG_PATH_INFO> applied_paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> applied_modes;
    const auto apply_activation = [&](const bool supply_target_mode) {
      auto attempt_paths = paths;
      auto attempt_modes = modes;
      if (supply_target_mode) {
        const auto target_mode_index = static_cast<UINT32>(attempt_modes.size());
        DISPLAYCONFIG_MODE_INFO target_mode {};
        target_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        target_mode.id = target.targetInfo.id;
        target_mode.adapterId = target.targetInfo.adapterId;
        target_mode.targetMode.targetVideoSignalInfo = target_signal;
        attempt_modes.push_back(target_mode);
        auto &attempt_target = attempt_paths.back();
        if (virtual_modes) {
          attempt_target.targetInfo.targetModeInfoIdx = target_mode_index;
        } else {
          attempt_target.targetInfo.modeInfoIdx = target_mode_index;
        }
      }
      const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE | SDC_ALLOW_CHANGES;
      if (win_api_->setDisplayConfig(attempt_paths, attempt_modes, flags | SDC_VALIDATE) != ERROR_SUCCESS || cancelled() ||
          win_api_->setDisplayConfig(attempt_paths, attempt_modes, flags | SDC_APPLY) != ERROR_SUCCESS || cancelled()) {
        return false;
      }
      applied_paths = std::move(attempt_paths);
      applied_modes = std::move(attempt_modes);
      return true;
    };
    if (!apply_activation(false) && (cancelled() || !apply_activation(true))) {
      return ApplyStatus::Retryable;
    }

    if (desired_hdr) {
      auto states = display_device_->getCurrentHdrStates({config.m_device_id});
      auto current = states.find(config.m_device_id);
      if (current == states.end() || current->second != desired_hdr) {
        if (cancelled() || !display_device_->setHdrStates({{config.m_device_id, desired_hdr}})) {
          return ApplyStatus::Retryable;
        }
        states = display_device_->getCurrentHdrStates({config.m_device_id});
        current = states.find(config.m_device_id);
        if (current == states.end() || current->second != desired_hdr || cancelled()) {
          return ApplyStatus::VerificationFailed;
        }
      }
    }

    const auto verified = win_api_->queryDisplayConfig(display_device::QueryType::Active);
    if (!verified || cancelled()) {
      return ApplyStatus::Retryable;
    }
    for (const auto &expected : applied_paths) {
      const auto found = std::find_if(verified->m_paths.begin(), verified->m_paths.end(), [&](const auto &path) {
        return same_luid(path.targetInfo.adapterId, expected.targetInfo.adapterId) &&
               path.targetInfo.id == expected.targetInfo.id && (path.flags & DISPLAYCONFIG_PATH_ACTIVE);
      });
      if (found == verified->m_paths.end()) {
        return ApplyStatus::VerificationFailed;
      }
      const auto before_index = display_device::win_utils::getSourceIndex(expected, applied_modes);
      const auto after_index = display_device::win_utils::getSourceIndex(*found, verified->m_modes);
      if (!before_index || !after_index) {
        return ApplyStatus::VerificationFailed;
      }
      const auto &before = applied_modes.at(*before_index).sourceMode;
      const auto &after = verified->m_modes.at(*after_index).sourceMode;
      const auto expected_rate = display_device::Rational {
        expected.targetInfo.refreshRate.Numerator,
        expected.targetInfo.refreshRate.Denominator,
      };
      const auto actual_rate = display_device::Rational {
        found->targetInfo.refreshRate.Numerator,
        found->targetInfo.refreshRate.Denominator,
      };
      if (before.width != after.width || before.height != after.height ||
          before.position.x != after.position.x || before.position.y != after.position.y ||
          expected.targetInfo.rotation != found->targetInfo.rotation ||
          !refresh_rates_match(expected_rate, actual_rate)) {
        return ApplyStatus::VerificationFailed;
      }
    }
    return ApplyStatus::Ok;
  }

  bool WinDisplaySettings::blank_hdr_states(std::chrono::milliseconds delay, const std::function<bool()> &cancelled) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (!ensure_initialized() || cancelled()) return false;
    std::lock_guard lock(settings_mutex_);
    const auto topology = display_device_->getCurrentTopology();
    std::set<std::string> ids;
    for (const auto &group : topology) ids.insert(group.begin(), group.end());
    const auto states = display_device_->getCurrentHdrStates(ids);
    display_device::HdrStateMap disabled, original;
    for (const auto &[id, state] : states) {
      if (state == display_device::HdrState::Enabled) {
        disabled[id] = display_device::HdrState::Disabled;
        original[id] = display_device::HdrState::Enabled;
      }
    }
    if (original.empty()) return true;
    if (cancelled()) return false;
    // Even a failing disable can partially mutate the outputs. Restore before
    // leaving this worker, including cancellation and exception paths.
    auto restore = util::fail_guard([&]() noexcept {
      try { display_device_->setHdrStates(original); } catch (...) {
        BOOST_LOG(error) << "Display helper: failed to restore HDR after cancelled blank";
      }
    });
    if (!display_device_->setHdrStates(disabled)) return false;
    const auto end = std::chrono::steady_clock::now() + delay;
    while (!cancelled() && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const bool restored = display_device_->setHdrStates(original);
    restore.disable();
    return restored && !cancelled();
  }

  ApplyStatus WinDisplaySettings::apply_topology(const ActiveTopology &topology) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return ApplyStatus::HelperUnavailable;
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

    // Keep legacy callers on the initialized device and preserve its list-shaped fallback.
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_device_->enumAvailableDevices(detail);
    } catch (...) {
      return {};
    }
  }

  std::optional<EnumeratedDeviceList> WinDisplaySettings::try_enumerate(
    display_device::DeviceEnumerationDetail detail) {
    if (!ensure_initialized()) {
      return std::nullopt;
    }

    // TopologyTransition owns the retry cadence; keep each poll short and
    // side-effect free instead of letting a query trigger nested recovery.
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // libdisplaydevice can report an empty list after QDC failure, or a partial
      // list after an available path fails identity mapping. Track both outcomes
      // so neither can be mistaken for a successful observation of no devices.
      struct EnumerationApi: display_device::WinApiLayer {
        using PathKey = std::tuple<LONG, DWORD, UINT32, LONG, DWORD, UINT32>;

        struct PathObservation {
          std::string monitor_path;
          std::string device_id;
          bool monitor_path_observed = false;
          bool device_id_observed = false;
          bool display_name_observed = false;
        };

        mutable bool query_available = false;
        mutable bool query_has_available_paths = false;
        mutable bool path_mapping_incomplete = false;
        mutable std::map<PathKey, PathObservation> path_observations;
        mutable std::vector<DISPLAYCONFIG_PATH_INFO> available_paths;

        PathKey path_key(const DISPLAYCONFIG_PATH_INFO &path) const {
          return {
            path.sourceInfo.adapterId.HighPart,
            path.sourceInfo.adapterId.LowPart,
            path.sourceInfo.id,
            path.targetInfo.adapterId.HighPart,
            path.targetInfo.adapterId.LowPart,
            path.targetInfo.id
          };
        }

        std::optional<display_device::PathAndModeData> queryDisplayConfig(
          display_device::QueryType type) const override {
          auto result = WinApiLayer::queryDisplayConfig(type);
          query_available = result.has_value();
          query_has_available_paths = false;
          path_mapping_incomplete = false;
          path_observations.clear();
          available_paths.clear();
          query_has_available_paths = result && std::any_of(
            result->m_paths.begin(),
            result->m_paths.end(),
            [](const auto &path) { return display_device::win_utils::isAvailable(path); }
          );
          if (result) {
            for (const auto &path : result->m_paths) {
              if (display_device::win_utils::isAvailable(path)) {
                const bool inserted = path_observations.try_emplace(path_key(path)).second;
                path_mapping_incomplete |= !inserted;
                available_paths.push_back(path);
              } else if (display_device::win_utils::isActive(path)) {
                path_mapping_incomplete = true;
              }
            }
          }
          return result;
        }

        std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const override {
          auto value = WinApiLayer::getMonitorDevicePath(path);
          if (display_device::win_utils::isAvailable(path)) {
            const auto observation = path_observations.find(path_key(path));
            if (observation == path_observations.end()) {
              path_mapping_incomplete = true;
            } else {
              observation->second.monitor_path_observed = true;
              observation->second.monitor_path = value;
              path_mapping_incomplete |= value.empty();
            }
          }
          return value;
        }

        std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const override {
          auto value = WinApiLayer::getDeviceId(path);
          if (display_device::win_utils::isAvailable(path)) {
            path_mapping_incomplete |= value.empty();
            const auto observation = path_observations.find(path_key(path));
            if (observation == path_observations.end()) {
              path_mapping_incomplete = true;
            } else {
              observation->second.device_id_observed = true;
              observation->second.device_id = value;
              if (const auto utf16_path = monitor_path_to_utf16(observation->second.monitor_path)) {
                path_mapping_incomplete |= enumeration_status::is_path_fallback_device_id(*utf16_path, value);
              } else {
                path_mapping_incomplete = true;
              }
              // libdisplaydevice resolves the target path again inside getDeviceId.
              // Reject observations whose path changed during those separate reads.
              const auto path_after_identity_lookup = WinApiLayer::getMonitorDevicePath(path);
              path_mapping_incomplete |= path_after_identity_lookup.empty() ||
                                         path_after_identity_lookup != observation->second.monitor_path;
            }
          }
          return value;
        }

        std::string getDisplayName(const DISPLAYCONFIG_PATH_INFO &path) const override {
          auto value = WinApiLayer::getDisplayName(path);
          if (display_device::win_utils::isAvailable(path)) {
            const auto observation = path_observations.find(path_key(path));
            if (observation == path_observations.end()) {
              path_mapping_incomplete = true;
            } else {
              observation->second.display_name_observed = true;
              path_mapping_incomplete |= value.empty();
            }
          }
          return value;
        }
      };
      auto api = std::make_shared<EnumerationApi>();
      display_device::WinDisplayDevice enumeration_device(api);
      const auto devices = enumeration_device.enumAvailableDevices(detail);
      std::vector<std::string> available_path_ids;
      for (const auto &path : api->available_paths) {
        const auto observation = api->path_observations.find(api->path_key(path));
        if (observation == api->path_observations.end() || !enumeration_status::is_complete_path_mapping(observation->second.monitor_path_observed, observation->second.device_id_observed, observation->second.display_name_observed)) {
          api->path_mapping_incomplete = true;
          continue;
        }
        available_path_ids.push_back(observation->second.device_id);
        const auto path_after_enumeration = api->WinApiLayer::getMonitorDevicePath(path);
        api->path_mapping_incomplete |= path_after_enumeration.empty() ||
                                        path_after_enumeration != observation->second.monitor_path;
      }
      std::vector<std::string> enumerated_device_ids;
      enumerated_device_ids.reserve(devices.size());
      for (const auto &device : devices) {
        enumerated_device_ids.push_back(device.m_device_id);
      }
      api->path_mapping_incomplete |= !enumeration_status::all_available_path_ids_were_enumerated(
        available_path_ids,
        enumerated_device_ids
      );
      if (!enumeration_status::is_complete_observation(
            api->query_available,
            api->query_has_available_paths,
            api->path_mapping_incomplete,
            devices.empty()
          )) {
        return std::nullopt;
      }
      return devices;
    } catch (...) {
      return std::nullopt;
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
      if (validate_topology_with_os(topology) != ApplyStatus::Ok) {
        BOOST_LOG(warning) << "Display helper v2: OS topology probe failed; preserving structurally valid restore snapshot for retry.";
      }
      return true;
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
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    return apply_snapshot_with_layouts(snapshot, nullptr);
  }

  bool WinDisplaySettings::apply_snapshot_settings(const Snapshot &snapshot) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (!ensure_initialized()) {
      BOOST_LOG(error) << "apply_snapshot_settings: display device not initialized";
      return false;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // Topology is staged by TopologyTransition. Restore exact modes before
      // primary/origin changes, and defer HDR until after rotations. Moving
      // primary first can make Windows reject a saved mode on a waking or
      // duplicate display, leaving recovery half-applied.
      if (!display_helper::restore_snapshot_settings(*display_device_, snapshot, false)) {
        BOOST_LOG(warning) << "apply_snapshot_settings: failed to restore settings; skipping remaining layout saves";
        return false;
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

  bool WinDisplaySettings::apply_snapshot_hdr_states(const Snapshot &snapshot) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (!ensure_initialized()) {
      return false;
    }
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      return display_helper::restore_snapshot_hdr_states(*display_device_, snapshot);
    } catch (...) {
      return false;
    }
  }

  bool WinDisplaySettings::apply_snapshot_with_layouts(const Snapshot &snapshot, const codec::layout_rotation_map_t *layout_rotations) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
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
      if (!apply_snapshot_hdr_states(snapshot)) {
        BOOST_LOG(warning) << "apply_snapshot: failed to restore final HDR states";
        return false;
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
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (!ensure_initialized()) {
      return false;
    }

    // Treat monitor reposition as part of APPLY semantics (no recovery).
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // Preserved positions are frequently already correct. Avoid another CCD
      // APPLY/database write (and its display notification) for a no-op.
      for (const auto &device : display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
        if (boost::iequals(device.m_device_id, device_id) && device.m_info && device.m_info->m_origin_point.m_x == origin.m_x && device.m_info->m_origin_point.m_y == origin.m_y) {
          return true;
        }
      }
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
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
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

  ApplyPreflightOutcome WinDisplaySettings::preflight_apply(
    const SingleDisplayConfiguration &config,
    const std::optional<ActiveTopology> &base_topology) {
    if (!ensure_initialized()) {
      return {.status = ApplyStatus::Retryable};
    }

    std::lock_guard lock(settings_mutex_);
    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // optional::value_or evaluates its fallback eagerly. Keep the supplied
      // Sunshine topology as the single preflight snapshot instead of issuing
      // another live QueryDisplayConfig call that cannot affect this branch.
      const auto topology_before = base_topology ? *base_topology : display_device_->getCurrentTopology();
      if (!display_device_->isTopologyValid(topology_before)) {
        return {.status = base_topology ? ApplyStatus::InvalidRequest : ApplyStatus::Retryable};
      }

      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      auto initial = topology::compute_initial_state(
        std::nullopt,
        topology_before,
        devices);
      if (!initial) {
        return {.status = ApplyStatus::Retryable};
      }

      auto [new_topology, device_to_configure, additional_devices] = topology::compute_new_topology_and_metadata(
        config.m_device_prep,
        config.m_device_id,
        *initial);
      const auto validation_status = display_device_->isTopologyTheSame(topology_before, new_topology) ?
                                       ApplyStatus::Ok :
                                       validate_topology_with_os(new_topology);
      if (validation_status != ApplyStatus::Ok) {
        return {.status = validation_status};
      }

      TopologyActivationTarget activation_target;
      activation_target.kind = config.m_device_id.empty() ?
                                 DeviceTargetKind::DefaultPrimaryGroup :
                                 DeviceTargetKind::ExplicitDevice;
      if (!device_to_configure.empty()) {
        activation_target.acceptable_device_ids.insert(std::move(device_to_configure));
      }
      if (activation_target.kind == DeviceTargetKind::DefaultPrimaryGroup) {
        activation_target.acceptable_device_ids.insert(additional_devices.begin(), additional_devices.end());
      }
      return ApplyPreflightOutcome {
        .status = ApplyStatus::Ok,
        .plan = ApplyTopologyPlan {
          .topology = std::move(new_topology),
          .activation_target = std::move(activation_target),
        },
      };
    } catch (...) {
      return {.status = ApplyStatus::Retryable};
    }
  }

  bool WinDisplaySettings::reset_staged_apply_state() {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (!ensure_initialized()) {
      return false;
    }

    std::lock_guard lock(settings_mutex_);
    if (!settings_manager_->resetPersistence()) {
      return false;
    }
    return true;
  }

  bool WinDisplaySettings::set_device_refresh_rate(const std::string &device_id, unsigned int num, unsigned int den) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
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

  std::set<std::string> WinDisplaySettings::set_device_refresh_rates(
    const std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> &overrides) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return {};
    std::set<std::string> applied;
    if (overrides.empty() || !ensure_initialized()) {
      return applied;
    }

    using RequestedRate = std::pair<std::string, std::pair<unsigned int, unsigned int>>;
    std::unordered_map<std::string, RequestedRate> requested_by_normalized_id;
    for (const auto &[device_id, rate] : overrides) {
      if (device_id.empty() || rate.first == 0 || rate.second == 0) {
        continue;
      }
      requested_by_normalized_id[codec::normalize_device_id(device_id)] = {device_id, rate};
    }
    if (requested_by_normalized_id.empty()) {
      return applied;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      // libdisplaydevice returns no mode map if any requested id is currently
      // unavailable. Filter through one Minimal enumeration first so one
      // sleeping physical monitor cannot prevent every active override.
      std::unordered_map<std::string, RequestedRate> active_requests;
      std::set<std::string> active_device_ids;
      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      for (const auto &device : devices) {
        if (device.m_device_id.empty() || !device.m_info) {
          continue;
        }
        const auto requested = requested_by_normalized_id.find(codec::normalize_device_id(device.m_device_id));
        if (requested == requested_by_normalized_id.end()) {
          continue;
        }
        active_device_ids.insert(device.m_device_id);
        active_requests.emplace(device.m_device_id, requested->second);
      }
      if (active_device_ids.empty()) {
        return applied;
      }

      auto current_modes = display_device_->getCurrentDisplayModes(active_device_ids);
      std::set<std::string> changed;
      for (const auto &[active_device_id, requested] : active_requests) {
        const auto &[original_device_id, rate] = requested;
        auto mode = current_modes.find(active_device_id);
        if (mode == current_modes.end()) {
          continue;
        }
        const auto &current = mode->second.m_refresh_rate;
        if (current.m_denominator != 0 &&
            static_cast<std::uint64_t>(current.m_numerator) * rate.second ==
              static_cast<std::uint64_t>(rate.first) * current.m_denominator) {
          applied.insert(original_device_id);
          continue;
        }
        mode->second.m_refresh_rate = display_device::Rational {rate.first, rate.second};
        changed.insert(original_device_id);
      }

      if (changed.empty()) {
        return applied;
      }
      if (display_device_->setDisplayModes(current_modes)) {
        applied.insert(changed.begin(), changed.end());
      }
    } catch (...) {
    }
    return applied;
  }

  std::unordered_map<std::string, std::optional<display_device::Resolution>>
  WinDisplaySettings::get_repositionable_display_resolutions(const std::set<std::string> &device_ids) {
    std::unordered_map<std::string, std::optional<display_device::Resolution>> result;
    if (device_ids.empty() || !ensure_initialized()) {
      return result;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    try {
      std::unordered_map<std::string, std::vector<std::string>> requested_by_normalized_id;
      for (const auto &device_id : device_ids) {
        if (!device_id.empty()) {
          requested_by_normalized_id[codec::normalize_device_id(device_id)].push_back(device_id);
        }
      }

      const auto devices = display_device_->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
      for (const auto &device : devices) {
        if (device.m_device_id.empty() || !device.m_info) {
          continue;
        }
        const auto requested = requested_by_normalized_id.find(codec::normalize_device_id(device.m_device_id));
        if (requested == requested_by_normalized_id.end()) {
          continue;
        }
        for (const auto &original_id : requested->second) {
          result.emplace(original_id, device.m_info->m_resolution);
        }
      }
    } catch (...) {
    }
    return result;
  }

  std::optional<display_device::Resolution> WinDisplaySettings::get_display_resolution(const std::string &device_id) {
    if (device_id.empty()) {
      return std::nullopt;
    }
    const auto displays = get_repositionable_display_resolutions({device_id});
    const auto display = displays.find(device_id);
    if (display != displays.end()) {
      return display->second;
    }
    return std::nullopt;
  }

  bool WinDisplaySettings::can_reposition_device(const std::string &device_id) {
    return !device_id.empty() && get_repositionable_display_resolutions({device_id}).contains(device_id);
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
        auto settings = std::make_unique<display_device::SettingsManager>(
          display,
          std::make_shared<display_device::NoopAudioContext>(),
          std::move(persistent_state),
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

  ApplyStatus WinDisplaySettings::validate_topology_with_os(const ActiveTopology &topology) const {
    if (!display_device_->isTopologyValid(topology)) {
      return ApplyStatus::InvalidRequest;
    }

    display_device::DisplayRecoveryBehaviorGuard recovery_guard(display_device::DisplayRecoveryBehavior::Skip);
    const auto original_data = win_api_->queryDisplayConfig(display_device::QueryType::All);
    if (!original_data) {
      return ApplyStatus::Retryable;
    }

    const auto path_data = display_device::win_utils::collectSourceDataForMatchingPaths(*win_api_, original_data->m_paths);
    if (path_data.empty()) {
      return ApplyStatus::Retryable;
    }

    auto paths = display_device::win_utils::makePathsForNewTopology(topology, path_data, original_data->m_paths);
    if (paths.empty()) {
      return ApplyStatus::Retryable;
    }

    UINT32 flags = SDC_VALIDATE | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE;
    LONG result = win_api_->setDisplayConfig(paths, {}, flags);
    if (result == ERROR_GEN_FAILURE) {
      flags = SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE;
      result = win_api_->setDisplayConfig(paths, {}, flags);
    }
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Display helper v2: topology validation failed: " << result;
      return ApplyStatus::InvalidRequest;
    }
    return ApplyStatus::Ok;
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

    // Enumerate devices directly rather than iterating the name map: that map
    // also holds lowercase alias keys for lookup, and iterating it persisted
    // duplicate rotation entries per display.
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
        if (auto rotation = read_display_rotation_degrees(display_name_w)) {
          out.emplace(id, *rotation);
        }
      }
    } catch (...) {
    }
    return out;
  }

  bool WinDisplaySettings::apply_layout_rotations(const codec::layout_rotation_map_t &layout_rotations) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
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

  bool WinDisplaySettings::reassert_layout_rotations(const codec::layout_rotation_map_t &layout_rotations) {
    auto borrow_mutation = display_helper::borrow::native_mutation_scope_t::enter(
      display_helper::borrow::helper_process().load(std::memory_order_acquire));
    if (!borrow_mutation) return false;
    if (layout_rotations.empty()) {
      return true;
    }
    if (!ensure_initialized()) {
      return false;
    }

    auto names = active_display_names_by_device_id();
    std::set<std::wstring> reasserted;  // legacy snapshots may carry case-alias duplicate keys
    bool all_ok = true;

    for (const auto &[device_id, rotation] : layout_rotations) {
      if (rotation == 0) {
        continue;
      }
      auto it = names.find(device_id);
      if (it == names.end()) {
        continue;
      }
      if (!reasserted.insert(it->second).second) {
        continue;
      }

      auto buf = alloc_full_devmode(it->second);
      if (buf.empty()) {
        all_ok = false;
        continue;
      }
      auto *mode = reinterpret_cast<DEVMODEW *>(buf.data());
      if (mode->dmDisplayOrientation == DMDO_DEFAULT) {
        continue;
      }

      // CDS_RESET forces the mode-set through to the driver even though every
      // requested value equals the current value; without it Windows dedupes
      // the call and a stale driver-side pointer transform is never rebuilt.
      const LONG result = ChangeDisplaySettingsExW(it->second.c_str(), mode, nullptr, CDS_UPDATEREGISTRY | CDS_RESET, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        BOOST_LOG(warning) << "Rotation reassert: ChangeDisplaySettingsEx failed for display "
                           << std::string(it->second.begin(), it->second.end())
                           << " (error=" << result << ")";
        all_ok = false;
      } else {
        BOOST_LOG(info) << "Rotation reassert: forced same-value rotation refresh for display "
                        << std::string(it->second.begin(), it->second.end())
                        << " (" << rotation << " degrees)";
      }
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

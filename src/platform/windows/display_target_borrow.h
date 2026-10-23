#pragma once

#ifdef _WIN32
#include "src/display_target_lease.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_helper_borrow_consumer.h"
#include "src/platform/windows/display_helper_borrow_native.h"
#include "src/platform/windows/ipc/display_settings_client.h"
#include "src/platform/windows/virtual_display.h"
#include "src/remote_session.h"
#include "src/uuid.h"
#include <array>

#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_api_recovery.h>
#include <display_device/windows/win_api_utils.h>
#include <display_device/windows/win_display_device.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <variant>

namespace platf::windows_borrow {
  namespace detail {
    inline std::optional<std::shared_ptr<void>> pin_helper_resource(const display_helper::borrow::result_t &reply) {
      const auto host_resources = VDISPLAY::retain_borrow_native_resources();
      if (!host_resources) return std::nullopt;
      if (reply.native_keepalive_handle == 0) return *host_resources;
      if (reply.native_keepalive_handle > std::numeric_limits<std::uintptr_t>::max()) return std::nullopt;
      HANDLE helper = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, reply.token.helper.pid);
      if (!helper) return std::nullopt;
      const auto actual = display_helper::borrow::process_context(helper, reply.token.helper.pid);
      HANDLE duplicate = nullptr;
      const bool pinned = actual && *actual == reply.token.helper && DuplicateHandle(helper,
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(reply.native_keepalive_handle)),
        GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
      CloseHandle(helper);
      if (!pinned || !duplicate || duplicate == INVALID_HANDLE_VALUE) return std::nullopt;
      auto helper_resource = std::shared_ptr<void>(duplicate, [](void *handle) { CloseHandle(static_cast<HANDLE>(handle)); });
      return std::static_pointer_cast<void>(std::make_shared<std::vector<std::shared_ptr<void>>>(
        std::initializer_list<std::shared_ptr<void>> {*host_resources, std::move(helper_resource)}));
    }

    inline std::optional<bool> observed_hdr(display_device::WinApiLayer &api,
        const display_device::EnumeratedDevice &device) {
      if (device.m_info && device.m_info->m_hdr_state)
        return *device.m_info->m_hdr_state == display_device::HdrState::Enabled;
      // The library uses nullopt for both unsupported HDR and failed queries.
      // Positively establish unsupported SDR; never turn a query error into
      // a false HDR bit that could admit the wrong existing output mode.
      const auto config = api.queryDisplayConfig(display_device::QueryType::All);
      if (!config) return std::nullopt;
      const auto sources = display_device::win_utils::collectSourceDataForMatchingPaths(api, config->m_paths);
      const auto found = sources.find(device.m_device_id);
      if (found == sources.end() || !found->second.m_active_source) return std::nullopt;
      const auto source = found->second.m_source_id_to_path_index.find(*found->second.m_active_source);
      if (source == found->second.m_source_id_to_path_index.end() || source->second >= config->m_paths.size()) return std::nullopt;
      const auto &path = config->m_paths[source->second];
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color {};
      color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      color.header.size = sizeof(color);
      color.header.adapterId = path.targetInfo.adapterId;
      color.header.id = path.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS || color.advancedColorSupported) return std::nullopt;
      return false;
    }

    inline bool exact_shared_owner(const std::string &device_id,const std::string &persisted_guid) {
      // The encoder-probe fallback and arbitrary per-client VDDs are not the
      // persisted shared-game owner. A malformed/missing identity never falls back.
      if(device_id.empty() || persisted_guid.size()!=36 || VDISPLAY::has_retained_ensure_display())return false;
      for(std::size_t i=0;i<persisted_guid.size();++i) {
        if(i==8||i==13||i==18||i==23){if(persisted_guid[i]!='-')return false;}
        else if(!((persisted_guid[i]>='0'&&persisted_guid[i]<='9') || (persisted_guid[i]>='a'&&persisted_guid[i]<='f') || (persisted_guid[i]>='A'&&persisted_guid[i]<='F')))return false;
      }
      std::array<std::uint8_t,16> expected{};
      try {const auto parsed=uuid_util::uuid_t::parse(persisted_guid);std::copy_n(parsed.b8,expected.size(),expected.begin());}catch(...){return false;}
      if(std::ranges::all_of(expected,[](auto value){return value==0;}))return false;
      const auto targets=VDISPLAY::tracked_display_cleanup_targets();unsigned matches=0;
      for(const auto &entry:targets) {
        if(entry.guid_bytes!=expected)continue;
        // One shared GUID cannot name two live devices/backends. Reject the
        // entire ambiguous owner instead of accepting either requested pair.
        if(entry.device_id!=device_id || entry.backend==VDISPLAY::ensure_display_backend_e::none ||
           !VDISPLAY::tracked_display_cleanup_target_matches(entry) || ++matches!=1)return false;
      }
      return matches==1;
    }

    inline std::optional<display_target::observation_t> read_existing_exact_target(const display_target::descriptor_t &target,const std::string &shared_guid_snapshot) {
      if (target.platform != display_target::platform_e::windows_device || target.exact_id.empty()) return std::nullopt;
      display_device::DisplayRecoveryBehaviorGuard recovery(display_device::DisplayRecoveryBehavior::Skip);
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice display(api);
      const auto devices = display.enumAvailableDevices(display_device::DeviceEnumerationDetail::Full);
      const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto &value) {
        return value.m_device_id == target.exact_id;
      });
      if (device == devices.end() || !device->m_info || device->m_display_name.empty()) return std::nullopt;
      const auto virtual_output = VDISPLAY::is_any_managed_virtual_display_output(target.exact_id);
      if (!virtual_output || *virtual_output != (target.kind == display_target::kind_e::shared_virtual)) return std::nullopt;
      if(target.kind==display_target::kind_e::shared_virtual && !exact_shared_owner(target.exact_id,shared_guid_snapshot))return std::nullopt;
      const auto hdr = observed_hdr(*api, *device);
      if (!hdr) return std::nullopt;
      const auto hz = std::visit([](const auto &value) -> double {
        using value_t = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_t, double>) return value;
        else return value.m_denominator ? static_cast<double>(value.m_numerator) / value.m_denominator : 0.0;
      }, device->m_info->m_refresh_rate);
      const auto millihz = hz * 1000.0;
      const auto width = device->m_info->m_resolution.m_width, height = device->m_info->m_resolution.m_height;
      if (!std::isfinite(millihz) || millihz < 1 || millihz > std::numeric_limits<std::uint32_t>::max() ||
          width == 0 || height == 0 || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) return std::nullopt;
      const auto outputs = platf::display_names(platf::mem_type_e::dxgi);
      const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto &name) {
        return _stricmp(name.c_str(), device->m_display_name.c_str()) == 0;
      });
      if (output == outputs.end()) return std::nullopt;
      return display_target::observation_t {target,
        {static_cast<int>(width), static_cast<int>(height), static_cast<std::uint32_t>(std::llround(millihz)), *hdr},
        *output, {}};
    }

    inline display_helper::borrow::consumer_t &consumer(const std::function<std::string()> &shared_guid_snapshot) {
      using namespace display_helper::borrow;
      static consumer_t value(process_gate(), consumer_ports_t {
        .context = current_context,
        .acquire = [](const request_t &request, auto deadline) {
          bool unknown = false;
          const auto reply = display_helper_client::acquire_borrow_quiescence(request, deadline, {}, unknown);
          return acquire_outcome_t {reply, unknown};
        },
        .release = [](const token_t &token, auto deadline) {
          return display_helper_client::release_borrow_quiescence(token, deadline);
        },
        .pin_native_resource = pin_helper_resource,
        .inhibit_kernel = [](const result_t &reply, auto deadline) -> std::shared_ptr<void> {
          bool unknown = false;
          auto hold = kernel_hold_t::acquire(reply.token.helper, deadline, unknown);
          if (unknown) {
            process_gate().retain_unknown();
            display_target::leases().fence_unknown_release();
          }
          return hold;
        },
        .alive = controller_alive,
        .observe = [shared_guid_snapshot](const auto &target) { return read_existing_exact_target(target,shared_guid_snapshot()); },
        .fence_unknown = [] { display_target::leases().fence_unknown_release(); },
      });
      return value;
    }
  }

  inline void register_exact_target_probe(std::function<std::string()> shared_guid_snapshot) {
    remote_session::register_exact_target_probe([shared_guid_snapshot=std::move(shared_guid_snapshot)](const display_target::descriptor_t &target) {
      if(display_helper::borrow::process_teardown_started().load(std::memory_order_acquire))
        return std::optional<display_target::observation_t>{};
      return detail::consumer(shared_guid_snapshot).observe(target);
    });
  }
}
#endif

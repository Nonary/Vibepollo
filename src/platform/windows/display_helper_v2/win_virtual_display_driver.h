#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <boost/algorithm/string/predicate.hpp>
#include <winioctl.h>

#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_api_recovery.h>
#include <display_device/windows/win_display_device.h>
#include <sudovda/sudovda.h>

namespace display_helper::v2 {
  class WinVirtualDisplayDriver final : public IVirtualDisplayDriver {
  public:
    ~WinVirtualDisplayDriver() override {
      close_handle();
    }

    bool disable() override {
      close_handle();
      return true;
    }

    bool enable() override {
      if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        return true;
      }
      handle_ = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID);
      return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

    bool is_available() override {
      HANDLE h = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID);
      if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return false;
      }
      CloseHandle(h);
      return true;
    }

    std::string device_id() override {
      try {
        display_device::DisplayRecoveryBehaviorGuard guard(display_device::DisplayRecoveryBehavior::Skip);
        auto api = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice dd(api);
        auto devices = dd.enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);

        auto is_virtual = [](const display_device::EnumeratedDevice &device) -> bool {
          static constexpr const char *kFriendly = "SudoMaker Virtual Display Adapter";
          if (!device.m_friendly_name.empty() && boost::iequals(device.m_friendly_name, kFriendly)) {
            return true;
          }

          if (device.m_edid) {
            // SudoVDA virtual displays ship a stable EDID (SMK/D1CE).
            if (boost::iequals(device.m_edid->m_manufacturer_id, "SMK") &&
                boost::iequals(device.m_edid->m_product_code, "D1CE")) {
              return true;
            }
          }

          return false;
        };

        const display_device::EnumeratedDevice *best = nullptr;
        for (const auto &device : devices) {
          if (!is_virtual(device)) {
            continue;
          }
          // Prefer active + primary if available.
          if (device.m_info && device.m_info->m_primary) {
            best = &device;
            break;
          }
          if (!best) {
            best = &device;
          } else if (!best->m_info && device.m_info) {
            best = &device;
          }
        }

        if (!best) {
          return {};
        }

        if (!best->m_device_id.empty()) {
          return best->m_device_id;
        }
        return best->m_display_name;
      } catch (...) {
        return {};
      }
    }

  private:
    void close_handle() {
      if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
      }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
  };
}  // namespace display_helper::v2

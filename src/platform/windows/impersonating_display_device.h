/**
 * @file src/platform/windows/impersonating_display_device.h
 * @brief WinDisplayDeviceInterface wrapper that impersonates the active user for each call.
 */
#pragma once

#ifdef _WIN32

  // third-party interfaces
  #include <display_device/windows/win_display_device_interface.h>

  // local includes
  #include "src/platform/windows/misc.h"
  #include "src/utility.h"

namespace display_device {
  class ImpersonatingDisplayDevice final: public WinDisplayDeviceInterface {
  public:
    explicit ImpersonatingDisplayDevice(std::shared_ptr<WinDisplayDeviceInterface> inner):
        m_inner(std::move(inner)) {}

    [[nodiscard]] bool isApiAccessAvailable() const override {
      return run<bool>([&] {
        return m_inner->isApiAccessAvailable();
      },
                       /*mutating=*/false);
    }

    [[nodiscard]] EnumeratedDeviceList enumAvailableDevices() const override {
      return run<EnumeratedDeviceList>([&] {
        return m_inner->enumAvailableDevices();
      },
                                       /*mutating=*/false);
    }

    [[nodiscard]] std::string getDisplayName(const std::string &device_id) const override {
      return run<std::string>([&] {
        return m_inner->getDisplayName(device_id);
      },
                              /*mutating=*/false);
    }

    [[nodiscard]] ActiveTopology getCurrentTopology() const override {
      return run<ActiveTopology>([&] {
        return m_inner->getCurrentTopology();
      },
                                 /*mutating=*/false);
    }

    [[nodiscard]] bool isTopologyValid(const ActiveTopology &topology) const override {
      return run<bool>([&] {
        return m_inner->isTopologyValid(topology);
      },
                       /*mutating=*/false);
    }

    [[nodiscard]] bool isTopologyTheSame(const ActiveTopology &lhs, const ActiveTopology &rhs) const override {
      return run<bool>([&] {
        return m_inner->isTopologyTheSame(lhs, rhs);
      },
                       /*mutating=*/false);
    }

    [[nodiscard]] bool setTopology(const ActiveTopology &new_topology) override {
      return run<bool>([&] {
        return m_inner->setTopology(new_topology);
      },
                       /*mutating=*/true);
    }

    [[nodiscard]] DeviceDisplayModeMap getCurrentDisplayModes(const std::set<std::string> &device_ids) const override {
      return run<DeviceDisplayModeMap>([&] {
        return m_inner->getCurrentDisplayModes(device_ids);
      },
                                       /*mutating=*/false);
    }

    [[nodiscard]] bool setDisplayModes(const DeviceDisplayModeMap &modes) override {
      return run<bool>([&] {
        return m_inner->setDisplayModes(modes);
      },
                       /*mutating=*/true);
    }

    [[nodiscard]] bool setDisplayModesTemporary(const DeviceDisplayModeMap &modes) override {
      return run<bool>([&] {
        return m_inner->setDisplayModesTemporary(modes);
      },
                       /*mutating=*/true);
    }

    [[nodiscard]] bool isPrimary(const std::string &device_id) const override {
      return run<bool>([&] {
        return m_inner->isPrimary(device_id);
      },
                       /*mutating=*/false);
    }

    [[nodiscard]] bool setAsPrimary(const std::string &device_id) override {
      return run<bool>([&] {
        return m_inner->setAsPrimary(device_id);
      },
                       /*mutating=*/true);
    }

    [[nodiscard]] HdrStateMap getCurrentHdrStates(const std::set<std::string> &device_ids) const override {
      return run<HdrStateMap>([&] {
        return m_inner->getCurrentHdrStates(device_ids);
      },
                              /*mutating=*/false);
    }

    [[nodiscard]] bool setHdrStates(const HdrStateMap &states) override {
      return run<bool>([&] {
        return m_inner->setHdrStates(states);
      },
                       /*mutating=*/true);
    }

    [[nodiscard]] bool restoreMonitorSettings() override {
      return run<bool>([&] {
        return m_inner->restoreMonitorSettings();
      },
                       /*mutating=*/true);
    }

  private:
    template<typename T, class Fn>
    T run(Fn &&fn, bool mutating) const {
      // If we are not running as SYSTEM, just call through.
      if (!platf::is_running_as_system()) {
        return std::forward<Fn>(fn)();
      }

      T result {};
      HANDLE token = platf::retrieve_users_token(/*elevated*/ true);
      if (!token) {
        // If we cannot impersonate for a mutating operation, avoid applying
        // changes under SYSTEM. Return a safe default and log.
        if (mutating) {
          // No logging facility available here directly; rely on callers/underlying
          // layers to surface errors due to failed operation.
          return result;
        }
        // For read-only operations, fall back to direct call.
        return std::forward<Fn>(fn)();
      }

      auto close_token = util::fail_guard([&]() {
        CloseHandle(token);
      });
      (void) platf::impersonate_current_user(token, [&]() {
        result = std::forward<Fn>(fn)();
      });
      return result;
    }

    std::shared_ptr<WinDisplayDeviceInterface> m_inner;
  };
}  // namespace display_device

#endif  // _WIN32

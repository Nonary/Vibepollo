#pragma once

#include <string>
#include <vector>
#include <Windows.h>

namespace service_ctrl {
  struct startup_state {
    bool status = false;
    bool installed = false;
    bool can_change = false;
    bool automatic = false;
    DWORD start_type = SERVICE_NO_CHANGE;
    DWORD error_code = ERROR_SUCCESS;
    std::string error;
  };

  namespace startup_detail {
    inline void fail(startup_state &state, DWORD code) {
      state.error_code = code;
      if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
        state.error = "The Windows service is not installed. Automatic startup requires an installed service.";
      } else if (code == ERROR_ACCESS_DENIED) {
        state.error = "Administrator permissions are required to change Windows service startup. Run the host as its installed service or as administrator.";
      } else {
        state.error = "Unable to read or change Windows service startup (Windows error " + std::to_string(code) + ").";
      }
    }

    class service {
    public:
      SC_HANDLE manager = nullptr;
      SC_HANDLE handle = nullptr;
      startup_state state;

      explicit service(DWORD access) {
        manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) {
          fail(state, GetLastError());
          return;
        }
        // This API controls only our installed service, never a client-supplied name.
        handle = OpenServiceW(manager, L"ApolloService", access);
        if (!handle) {
          fail(state, GetLastError());
          return;
        }
        state.installed = true;
      }

      service(const service &) = delete;
      service &operator=(const service &) = delete;

      ~service() {
        if (handle) {
          CloseServiceHandle(handle);
        }
        if (manager) {
          CloseServiceHandle(manager);
        }
      }

      bool read() {
        if (!handle) {
          return false;
        }
        DWORD bytes = 0;
        QueryServiceConfigW(handle, nullptr, 0, &bytes);
        const auto error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(QUERY_SERVICE_CONFIGW)) {
          fail(state, error == ERROR_INSUFFICIENT_BUFFER ? ERROR_INVALID_DATA : error);
          return false;
        }
        std::vector<unsigned char> buffer(bytes);
        auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
        if (!QueryServiceConfigW(handle, config, bytes, &bytes)) {
          fail(state, GetLastError());
          return false;
        }
        state.start_type = config->dwStartType;
        state.automatic = config->dwStartType == SERVICE_AUTO_START;
        state.status = true;
        return true;
      }
    };
  }  // namespace startup_detail

  inline startup_state get_service_startup() {
    startup_detail::service service {SERVICE_QUERY_CONFIG};
    if (service.read()) {
      auto change = OpenServiceW(service.manager, L"ApolloService", SERVICE_CHANGE_CONFIG);
      service.state.can_change = change != nullptr;
      if (change) {
        CloseServiceHandle(change);
      } else {
        startup_detail::fail(service.state, GetLastError());
      }
    }
    return service.state;
  }

  inline startup_state set_service_startup(bool automatic) {
    startup_detail::service service {SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG};
    if (!service.read()) {
      return service.state;
    }
    service.state.can_change = true;
    const DWORD desired = automatic ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
    if (service.state.start_type == desired) {
      return service.state;
    }
    // Manual startup keeps the app launchable. Only future startup changes:
    // do not stop/start the service, alter its account, or disable it.
    if (!ChangeServiceConfigW(service.handle, SERVICE_NO_CHANGE, desired, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
      service.state.status = false;
      startup_detail::fail(service.state, GetLastError());
      return service.state;
    }
    service.state.status = false;
    if (service.read() && service.state.start_type != desired) {
      service.state.status = false;
      startup_detail::fail(service.state, ERROR_WRITE_FAULT);
    }
    return service.state;
  }
}  // namespace service_ctrl

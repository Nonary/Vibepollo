#include "src/platform/windows/display_helper_v2/win_event_pump.h"
#include "src/color_profile_policy.h"
#include "src/logging.h"

#include <algorithm>
#include <cstddef>
#include <dbt.h>
#include <powrprof.h>

namespace display_helper::v2 {
  namespace {
    const GUID kMonitorInterfaceGuid = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
  }

  void WinEventPump::start(Callback callback) {
    stop();
    device_diagnostics_ = {};
    callback_ = std::move(callback);
    worker_ = std::jthread(&WinEventPump::thread_proc, this);
  }

  void WinEventPump::stop() {
    if (worker_.joinable()) {
      if (HWND hwnd = hwnd_.load(std::memory_order_acquire)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      }
      worker_.request_stop();
      worker_.join();
    }
    callback_ = nullptr;
    hwnd_.store(nullptr, std::memory_order_release);
  }

  WinEventPump::~WinEventPump() {
    stop();
  }

  LRESULT CALLBACK WinEventPump::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
      auto *self = static_cast<WinEventPump *>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->hwnd_.store(hwnd, std::memory_order_release);
      return TRUE;
    }

    auto *self = reinterpret_cast<WinEventPump *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
      case WM_DISPLAYCHANGE:
        self->signal(DisplayEvent::DisplayChange);
        break;
      case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL) {
          self->log_device_event(wParam, lParam);
          self->signal(DisplayEvent::DeviceArrival);
        } else if (wParam == DBT_DEVICEREMOVECOMPLETE) {
          self->log_device_event(wParam, lParam);
          self->signal(DisplayEvent::DeviceRemoval);
        } else if (wParam == DBT_DEVNODES_CHANGED) {
          self->signal(DisplayEvent::DisplayChange);
        }
        break;
      case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) {
          self->signal(DisplayEvent::PowerResume);
        } else if (wParam == PBT_POWERSETTINGCHANGE) {
          const auto *ps = reinterpret_cast<const POWERBROADCAST_SETTING *>(lParam);
          if (ps && ps->PowerSetting == GUID_MONITOR_POWER_ON && ps->DataLength == sizeof(DWORD)) {
            const DWORD state = *reinterpret_cast<const DWORD *>(ps->Data);
            if (self->monitor_power_policy_.observe(true, state != 0)) {
              self->signal(DisplayEvent::PowerResume);
            }
          } else {
            (void) self->monitor_power_policy_.observe(false, false);
          }
        }
        break;
      case WM_DESTROY:
        self->cleanup_notifications();
        PostQuitMessage(0);
        break;
      default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void WinEventPump::signal(DisplayEvent event) {
    if (callback_) {
      callback_(event);
    }
  }

  void WinEventPump::log_device_event(WPARAM event, LPARAM payload) noexcept {
    try {
      const auto admitted = device_diagnostics_.admit(device_diagnostics::context_t::clock::now());
      if (!admitted) return;
      const char *type = "missing", *state = "missing", *interface_class = "n/a", *family = "unknown";
      std::string model = "unknown";
      unsigned identity = 0;
      DWORD type_id = 0xffffffff;
      const auto *header = reinterpret_cast<const DEV_BROADCAST_HDR *>(payload);
      // The OS owns the payload for this callback. Validate its declared bounds
      // before inspecting a variant or its variable-length UTF-16 name.
      if (header) {
        state = "invalid";
        if (header->dbch_size >= sizeof(DEV_BROADCAST_HDR) && header->dbch_size <= 65536) {
          state = "valid";
          type_id = header->dbch_devicetype;
          const auto bounded_name = [&](const wchar_t *name, std::size_t offset) -> std::wstring_view {
            if (header->dbch_size < offset + sizeof(wchar_t)) { state = "invalid"; return {}; }
            const auto count = std::min<std::size_t>((header->dbch_size - offset) / sizeof(wchar_t), device_diagnostics::context_t::max_name + 1);
            const auto end = std::find(name, name + count, L'\0');
            if (end == name + count) { state = "unterminated-or-long"; return {}; }
            return {name, static_cast<std::size_t>(end - name)};
          };
          switch (header->dbch_devicetype) {
            case DBT_DEVTYP_DEVICEINTERFACE:
              type = "interface";
              if (header->dbch_size >= offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) + sizeof(wchar_t)) {
                const auto *item = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W *>(header);
                const bool monitor = item->dbcc_classguid == kMonitorInterfaceGuid;
                interface_class = monitor ? "monitor" : "other";
                const auto name = bounded_name(item->dbcc_name, offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name));
                identity = device_diagnostics_.identity(monitor ? L'M' : L'I', name);
                if (monitor && !name.empty()) {
                  model = device_diagnostics::monitor_model(name);
                  family = color_profile_policy::is_sunshine_driver_monitor_path(name) ? "managed-virtual" : "other";
                }
              } else { state = "invalid"; }
              break;
            case DBT_DEVTYP_PORT:
              type = "port";
              if (header->dbch_size >= offsetof(DEV_BROADCAST_PORT_W, dbcp_name) + sizeof(wchar_t)) {
                const auto *item = reinterpret_cast<const DEV_BROADCAST_PORT_W *>(header);
                identity = device_diagnostics_.identity(L'P', bounded_name(item->dbcp_name, offsetof(DEV_BROADCAST_PORT_W, dbcp_name)));
              } else { state = "invalid"; }
              break;
            case DBT_DEVTYP_VOLUME:
              type = "volume";
              if (header->dbch_size >= sizeof(DEV_BROADCAST_VOLUME)) {
                const auto *item = reinterpret_cast<const DEV_BROADCAST_VOLUME *>(header);
                identity = device_diagnostics_.identity(L'V', std::to_wstring(item->dbcv_unitmask));
              } else { state = "invalid"; }
              break;
            default:
              type = "other";
              break;
          }
        }
      }
      BOOST_LOG(debug) << "Display notification: event=" << (event == DBT_DEVICEARRIVAL ? "arrival" : "removal")
                       << " type=" << type << " type_id=" << type_id << " class=" << interface_class << " family=" << family
                       << " model=" << model << " identity=" << identity << " payload=" << state
                       << " suppressed=" << *admitted;
    } catch (...) {
      // Diagnostics must never prevent the existing event from being routed.
    }
  }

  void WinEventPump::cleanup_notifications() {
    if (power_cookie_) {
      UnregisterPowerSettingNotification(power_cookie_);
      power_cookie_ = nullptr;
    }
    if (device_cookie_) {
      UnregisterDeviceNotification(device_cookie_);
      device_cookie_ = nullptr;
    }
  }

  void WinEventPump::thread_proc(std::stop_token st) {
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t *klass = L"SunshineDisplayEventWindow";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WinEventPump::wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = klass;
    RegisterClassExW(&wc);

    // A message-only window does not receive broadcast WM_DISPLAYCHANGE.
    // Keep a hidden top-level window so the service can observe display
    // topology changes without showing UI or installing input hooks.
    HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      klass,
      L"",
      WS_POPUP,
      0,
      0,
      0,
      0,
      nullptr,
      nullptr,
      instance,
      this);
    if (!hwnd) {
      return;
    }
    monitor_power_policy_.reset();

    power_cookie_ = RegisterPowerSettingNotification(hwnd, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);

    DEV_BROADCAST_DEVICEINTERFACE_W dbi = {};
    dbi.dbcc_size = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid = kMonitorInterfaceGuid;
    device_cookie_ = RegisterDeviceNotificationW(hwnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);

    MSG msg;
    while (!st.stop_requested()) {
      const BOOL res = GetMessageW(&msg, nullptr, 0, 0);
      if (res == -1 || res == 0) {
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    cleanup_notifications();
    if (hwnd) {
      DestroyWindow(hwnd);
    }
    hwnd_.store(nullptr, std::memory_order_release);
  }
}  // namespace display_helper::v2

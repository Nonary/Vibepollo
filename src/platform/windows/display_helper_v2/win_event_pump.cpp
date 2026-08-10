#include "src/platform/windows/display_helper_v2/win_event_pump.h"

#include <dbt.h>
#include <powrprof.h>

namespace display_helper::v2 {
  namespace {
    const GUID kMonitorInterfaceGuid = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
  }

  void WinEventPump::start(Callback callback) {
    stop();
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
          self->signal(DisplayEvent::DeviceArrival);
        } else if (wParam == DBT_DEVICEREMOVECOMPLETE) {
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
            if (state != 0) {
              self->signal(DisplayEvent::PowerResume);
            }
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

    HWND hwnd = CreateWindowExW(0, klass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!hwnd) {
      return;
    }

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

/** A desktop receipt fence for the existing UMDF mouse collections.
 * Queue admission and VHF read credit are not Windows input delivery. Fence
 * Windows receipts before crossing mouse queues. Same-collection movement
 * stays asynchronous. No cursor polling, key-state polling, or event replay.
 */
#pragma once

#include <Windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platf::vhf_keyboard_mouse {
  class mouse_receipt {
  public:
    struct expectation {
      bool absolute {};
      bool motion {};
      LONG x {}, y {};
      USHORT buttons {};
      SHORT wheel {}, pan {};
      ULONG_PTR injected {};
    };

    ~mouse_receipt() { stop(); }

    static bool input_desktop_is_default() {
      const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!desktop) return false;
      const bool is_default = desktop_is_default(desktop);
      CloseDesktop(desktop);
      return is_default;
    }

    // The caller serializes start/stop and input submissions. The observer's
    // message thread can use the receipt state until stop() joins it.
    bool start(bool default_desktop_only = false) {
      if (worker.joinable()) return false;
      worker = std::thread([this, default_desktop_only] { run(default_desktop_only); });
      std::unique_lock lock(mutex);
      changed.wait(lock, [this] { return initialized; });
      return window != nullptr && hook != nullptr;
    }

    void stop() {
      HWND target;
      { std::lock_guard lock(mutex); target = window; }
      if (target) PostMessageW(target, WM_CLOSE, 0, 0);
      if (worker.joinable()) worker.join();

      std::lock_guard lock(mutex);
      window = nullptr;
      hook = nullptr;
      relative_device = nullptr;
      absolute_device = nullptr;
      pending.clear();
      counters = {};
      initialized = false;
      removed = false;
    }

    // Called after OPEN, before admitting any input. Resolve only these two
    // collections from the producer's dedicated keyboard/mouse container.
    bool bind() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      do {
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != UINT(-1) && count <= 4096) {
          std::vector<RAWINPUTDEVICELIST> devices(count);
          const UINT received = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
          HANDLE relative = nullptr, absolute = nullptr;
          bool duplicate = false;
          if (received != UINT(-1)) for (UINT i = 0; i < received; ++i) {
            if (devices[i].dwType != RIM_TYPEMOUSE) continue;
            const int collection = owned_collection(devices[i].hDevice);
            auto &target = collection == 2 ? relative : absolute;
            if (collection == 2 || collection == 3) {
              if (target != nullptr) duplicate = true;
              target = devices[i].hDevice;
            }
          }
          if (!duplicate && relative != nullptr && absolute != nullptr) {
            std::lock_guard lock(mutex);
            relative_device = relative;
            absolute_device = absolute;
            removed = false;
            return true;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    struct statistics {
      std::uint64_t operations {}, barriers {}, blocked_microseconds {}, max_blocked_microseconds {}, coalesced {};
      std::size_t peak_pending {};
    };

    statistics stats() {
      std::lock_guard lock(mutex);
      return counters;
    }

    // A collection already serializes its own reports. Wait only when crossing
    // collections/backends, when the bounded receipt queue is full, or when an
    // absolute position could alias an outstanding receipt. Neither
    // a cursor move nor a successful IOCTL is evidence that a crossing is safe.
    bool arm(expectation value) {
      std::unique_lock lock(mutex);
      const auto same_source = [](const expectation &a, const expectation &b) {
        return a.absolute == b.absolute && (a.injected != 0) == (b.injected != 0);
      };
      const auto ready = [&] {
        // Raw Input can coalesce absolute positions. Keep outstanding positions
        // distinguishable, so a repeated A in A/B/A cannot acknowledge a later
        // report before it actually reaches Windows.
        const bool aliases = value.absolute && std::any_of(pending.begin(), pending.end(), [&](const auto &p) {
          return p.absolute && p.x >= value.x - 6 && p.x <= value.x + 6 && p.y >= value.y - 6 && p.y <= value.y + 6;
        });
        return removed || (pending.size() < 64 && !aliases && (pending.empty() || same_source(pending.back(), value)));
      };
      if (!ready()) {
        ++counters.barriers;
        const auto started = std::chrono::steady_clock::now();
        const bool signaled = changed.wait_for(lock, std::chrono::milliseconds(100), ready);
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        counters.blocked_microseconds += elapsed;
        counters.max_blocked_microseconds = (std::max)(counters.max_blocked_microseconds, elapsed);
        if (!signaled) return false;
      }
      if (removed) return false;
      pending.push_back(value);
      ++counters.operations;
      counters.peak_pending = (std::max)(counters.peak_pending, pending.size());
      return true;
    }

    bool wait() {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, std::chrono::milliseconds(100), [this] { return pending.empty() || removed; }) && !removed;
    }

    // SendInput does not produce Raw Input. A unique extra-information tag is
    // acknowledged by the desktop's low-level hook instead. The hook calls the
    // rest of the chain first, and never suppresses or changes input.
    bool inject_absolute(std::uint16_t x, std::uint16_t y) {
      const ULONG_PTR token = next_injection_tag();
      expectation value {};
      value.injected = token;
      if (!arm(value)) return false;
      INPUT input {};
      input.type = INPUT_MOUSE;
      input.mi.dx = x;
      input.mi.dy = y;
      input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      input.mi.dwExtraInfo = token;
      if (SendInput(1, &input, sizeof(input)) != 1) return false;
      return true;
    }

  private:
    friend struct mouse_receipt_test;
    static bool desktop_is_default(HDESK desktop) {
      wchar_t name[256] {};
      DWORD needed = 0;
      return GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) &&
             _wcsicmp(name, L"Default") == 0;
    }

    inline static std::atomic<std::uint32_t> injection_sequence {GetTickCount() ^ (GetCurrentProcessId() * 0x9e3779b9u)};
    static ULONG_PTR next_injection_tag() {
      // MOUSE_INPUT_DATA carries a 32-bit ExtraInformation value. A wider
      // MOUSEINPUT tag can be truncated before the low-level hook receives it.
      // Keep tags process-wide so replacing an observer cannot reuse a token.
      auto tag = injection_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
      if (tag == 0) tag = injection_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
      return tag;
    }
    // SDK property-key values, defined locally so this header does not depend
    // on another translation unit defining INITGUID/exporting DEVPKEY symbols.
    inline static constexpr DEVPROPKEY instance_key {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
    inline static constexpr DEVPROPKEY container_key {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
    static int owned_collection(HANDLE device) {
      UINT length = 0;
      if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &length) == UINT(-1) || length == 0 || length > 32768) return 0;
      std::wstring path(length, L'\0');
      if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &length) == UINT(-1)) return 0;
      wchar_t instance[MAX_DEVICE_ID_LEN] {};
      DEVPROPTYPE type = 0;
      ULONG bytes = sizeof(instance);
      if (CM_Get_Device_Interface_PropertyW(path.c_str(), &instance_key, &type,
          reinterpret_cast<PBYTE>(instance), &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING ||
          bytes < sizeof(wchar_t) || bytes > sizeof(instance) || instance[bytes / sizeof(wchar_t) - 1] != L'\0') return 0;
      DEVINST node = 0;
      if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 0;
      constexpr GUID container {0x5d43cc5c, 0x318a, 0x46f4, {0x8a, 0x8f, 0x2d, 0x1d, 0x3c, 0x99, 0x80, 0x21}};
      GUID actual {};
      bytes = sizeof(actual);
      if (CM_Get_DevNode_PropertyW(node, &container_key, &type,
          reinterpret_cast<PBYTE>(&actual), &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID ||
          bytes != sizeof(actual) || !IsEqualGUID(actual, container)) return 0;
      // VHF's 0/0 identity produces HID_DEVICE_SYSTEM_VHF, not VID_0000.
      // Require the dedicated named parent as well as the container and TLC.
      DEVINST parent = 0;
      wchar_t parent_id[MAX_DEVICE_ID_LEN] {};
      if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS ||
          CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return 0;
      return collection_identity(instance, parent_id);
    }

    static int collection_identity(const wchar_t *instance, const wchar_t *parent) {
      constexpr wchar_t prefix[] = L"VHF\\HID_DEVICE_SYSTEM_VHF\\";
      const auto *suffix = wcsrchr(parent, L'&');
      if (_wcsnicmp(parent, prefix, std::size(prefix) - 1) != 0 || suffix == nullptr ||
          _wcsicmp(suffix + 1, L"VibeshineKeyboardMouse") != 0) return 0;
      constexpr wchar_t relative[] = L"HID\\HID_DEVICE_SYSTEM_VHF&Col02\\";
      constexpr wchar_t absolute[] = L"HID\\HID_DEVICE_SYSTEM_VHF&Col03\\";
      if (_wcsnicmp(instance, relative, std::size(relative) - 1) == 0) return 2;
      if (_wcsnicmp(instance, absolute, std::size(absolute) - 1) == 0) return 3;
      return 0;
    }

    void raw_input(HRAWINPUT handle) {
      RAWINPUT input {};
      UINT size = sizeof(input);
      if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
          size < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) || input.header.dwType != RIM_TYPEMOUSE) return;
      observe(input);
    }

    void observe(const RAWINPUT &input) {
      std::lock_guard lock(mutex);
      if (pending.empty() || removed) return;
      auto &expected = pending.front();
      if (expected.injected || input.header.hDevice != (expected.absolute ? absolute_device : relative_device)) return;
      const auto &mouse = input.data.mouse;
      if (((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) != expected.absolute) return;
      if (expected.absolute) {
        // A receipt for a later absolute position proves its preceding reports
        // on this same ordered HID collection were consumed/coalesced. Requiring
        // a separate WM_INPUT for every position would stall normal fast motion.
        const auto match = std::find_if(pending.begin(), pending.end(), [&](const auto &p) {
          return p.absolute && !p.injected && mouse.lLastX >= p.x - 3 && mouse.lLastX <= p.x + 3 &&
            mouse.lLastY >= p.y - 3 && mouse.lLastY <= p.y + 3;
        });
        if (match != pending.end()) {
          counters.coalesced += std::distance(pending.begin(), match);
          pending.erase(pending.begin(), std::next(match));
          changed.notify_all();
        }
        return;
      }
      if (expected.motion) {
        // Mouhid scales the producer's 15-bit absolute HID range. Permit its
        // integer rounding, never use desktop cursor position as a receipt.
        const LONG tolerance = expected.absolute ? 3 : 0;
        if (mouse.lLastX >= expected.x - tolerance && mouse.lLastX <= expected.x + tolerance &&
            mouse.lLastY >= expected.y - tolerance && mouse.lLastY <= expected.y + tolerance) expected.motion = false;
      }
      expected.buttons &= ~mouse.usButtonFlags;
      if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) && static_cast<SHORT>(mouse.usButtonData) == expected.wheel) expected.wheel = 0;
      if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) && static_cast<SHORT>(mouse.usButtonData) == expected.pan) expected.pan = 0;
      if (!expected.motion && expected.buttons == 0 && expected.wheel == 0 && expected.pan == 0) {
        pending.pop_front();
        changed.notify_all();
      }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      auto *self = reinterpret_cast<mouse_receipt *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (message == WM_NCCREATE) {
        self = static_cast<mouse_receipt *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }
      if (self && message == WM_INPUT) self->raw_input(reinterpret_cast<HRAWINPUT>(lparam));
      if (self && message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL) {
        std::lock_guard lock(self->mutex);
        const HANDLE device = reinterpret_cast<HANDLE>(lparam);
        if (device == self->relative_device || device == self->absolute_device) {
          self->removed = true;
          self->changed.notify_all();
        }
      }
      if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
      if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
      return DefWindowProcW(window, message, wparam, lparam);
    }

    inline static thread_local mouse_receipt *observer = nullptr;
    static LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
      const auto tag = code == HC_ACTION ? reinterpret_cast<MSLLHOOKSTRUCT *>(lparam)->dwExtraInfo : 0;
      const auto flags = code == HC_ACTION ? reinterpret_cast<MSLLHOOKSTRUCT *>(lparam)->flags : 0;
      const LRESULT result = CallNextHookEx(nullptr, code, wparam, lparam);
      if (observer && result == 0 && code == HC_ACTION && wparam == WM_MOUSEMOVE && (flags & LLMHF_INJECTED)) {
        std::lock_guard lock(observer->mutex);
        if (!observer->pending.empty() && observer->pending.front().injected == tag && tag != 0) {
          observer->pending.pop_front();
          observer->changed.notify_all();
        }
      }
      return result;
    }

    void run(bool default_desktop_only) {
      // Raw Input and the hook must live on the active input desktop. Later
      // desktop switches fail closed at the bounded receipt wait; recovery
      // deletes the HID child before subsequent ordinary Windows input.
      const HDESK original_desktop = GetThreadDesktop(GetCurrentThreadId());
      HDESK desktop = OpenInputDesktop(0, FALSE,
        DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL);
      const bool desktop_ready = desktop && (!default_desktop_only || desktop_is_default(desktop)) && SetThreadDesktop(desktop);
      const HINSTANCE module = GetModuleHandleW(nullptr);
      WNDCLASSW cls {};
      cls.lpfnWndProc = procedure;
      cls.hInstance = module;
      cls.lpszClassName = L"VibepolloVhfMouseReceipt";
      RegisterClassW(&cls);
      HWND created = desktop_ready ? CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, this) : nullptr;
      RAWINPUTDEVICE registration {0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, created};
      if (created && !RegisterRawInputDevices(&registration, 1, sizeof(registration))) { DestroyWindow(created); created = nullptr; }
      observer = this;
      HHOOK installed = created ? SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, module, 0) : nullptr;
      {
        std::lock_guard lock(mutex);
        window = installed ? created : nullptr;
        hook = installed;
        initialized = true;
        changed.notify_all();
      }
      if (created && installed) {
        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
      }
      if (installed) UnhookWindowsHookEx(installed);
      if (created) {
        RAWINPUTDEVICE remove {0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&remove, 1, sizeof(remove));
        if (IsWindow(created)) DestroyWindow(created);
      }
      observer = nullptr;
      // Restore before closing the handle of the former assigned desktop.
      if (desktop) { SetThreadDesktop(original_desktop); CloseDesktop(desktop); }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    HWND window {};
    HHOOK hook {};
    HANDLE relative_device {}, absolute_device {};
    std::deque<expectation> pending;
    statistics counters;
    bool initialized {}, removed {};
  };
}

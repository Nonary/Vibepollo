/**
 * @file src/platform/windows/hotkey_manager.cpp
 * @brief Global hotkey registration for Windows.
 */
#ifdef _WIN32
#include "hotkey_manager.h"

#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <ios>
#include <mutex>
#include <thread>

#include "display_helper_integration.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/virtual_display.h"

using namespace std::literals;

namespace {
  constexpr UINT kMsgUpdateHotkey = WM_APP + 1;
  constexpr UINT kMsgShutdown = WM_APP + 2;
  constexpr int kRestoreHotkeyId = 1;

  std::mutex &hotkey_mutex() {
    static std::mutex m;
    return m;
  }

  DWORD g_hotkey_thread_id = 0;
  bool g_hotkey_thread_started = false;
  bool g_hotkey_registered = false;
  int g_current_hotkey_vk = 0;
  UINT g_current_hotkey_modifiers = 0;
  std::atomic<bool> g_warned_system {false};

  void register_restore_hotkey_locked(int vk_code, UINT modifiers) {
    if (g_hotkey_registered) {
      UnregisterHotKey(nullptr, kRestoreHotkeyId);
      g_hotkey_registered = false;
    }

    g_current_hotkey_vk = vk_code;
    g_current_hotkey_modifiers = modifiers;
    if (vk_code <= 0) {
      return;
    }

#ifdef MOD_NOREPEAT
    if (modifiers != 0) {
      modifiers |= MOD_NOREPEAT;
    }
#endif
    if (!RegisterHotKey(nullptr, kRestoreHotkeyId, modifiers, static_cast<UINT>(vk_code))) {
      BOOST_LOG(warning) << "Failed to register restore hotkey (VK "sv << vk_code
                         << "): "sv << GetLastError();
      return;
    }

    g_hotkey_registered = true;
    BOOST_LOG(info) << "Registered restore hotkey (VK "sv << vk_code << ", modifiers 0x"
                    << std::hex << modifiers << std::dec << ").";
  }

  void trigger_restore() {
    BOOST_LOG(info) << "Restore hotkey triggered; reverting display configuration.";
    const bool reverted = display_helper_integration::revert();
    (void) VDISPLAY::removeAllVirtualDisplays();
    if (reverted) {
      display_helper_integration::stop_watchdog();
    }
  }

  void hotkey_thread_main(int initial_vk, UINT initial_modifiers, HANDLE ready_event) {
    MSG msg;
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    {
      std::lock_guard<std::mutex> lock(hotkey_mutex());
      g_hotkey_thread_id = GetCurrentThreadId();
      g_hotkey_thread_started = true;
    }

    SetEvent(ready_event);

    {
      std::lock_guard<std::mutex> lock(hotkey_mutex());
      register_restore_hotkey_locked(initial_vk, initial_modifiers);
    }

    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == WM_HOTKEY && msg.wParam == kRestoreHotkeyId) {
        trigger_restore();
        continue;
      }

      if (msg.message == kMsgUpdateHotkey) {
        const int new_vk = static_cast<int>(msg.wParam);
        const UINT new_modifiers = static_cast<UINT>(msg.lParam);
        std::lock_guard<std::mutex> lock(hotkey_mutex());
        if (new_vk != g_current_hotkey_vk || new_modifiers != g_current_hotkey_modifiers) {
          register_restore_hotkey_locked(new_vk, new_modifiers);
        }
        continue;
      }

      if (msg.message == kMsgShutdown) {
        break;
      }
    }

    std::lock_guard<std::mutex> lock(hotkey_mutex());
    if (g_hotkey_registered) {
      UnregisterHotKey(nullptr, kRestoreHotkeyId);
      g_hotkey_registered = false;
    }
  }
}  // namespace

namespace platf::hotkey {
  void update_restore_hotkey(int vk_code, unsigned int modifiers) {
    if (vk_code < 0) {
      vk_code = 0;
    }

    if (platf::is_running_as_system() && !g_warned_system.exchange(true)) {
      BOOST_LOG(warning) << "Restore hotkey registration may fail while running as SYSTEM "
                            "(no interactive session).";
    }

    std::unique_lock<std::mutex> lock(hotkey_mutex());
    if (!g_hotkey_thread_started) {
      if (vk_code == 0) {
        g_current_hotkey_vk = 0;
        g_current_hotkey_modifiers = modifiers;
        return;
      }

      HANDLE ready_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
      if (!ready_event) {
        BOOST_LOG(warning) << "Failed to create restore hotkey event: "sv << GetLastError();
        return;
      }

      std::thread([vk_code, modifiers, ready_event]() {
        hotkey_thread_main(vk_code, modifiers, ready_event);
      }).detach();

      lock.unlock();
      WaitForSingleObject(ready_event, 5000);
      CloseHandle(ready_event);
      return;
    }

    const DWORD thread_id = g_hotkey_thread_id;
    lock.unlock();

    if (thread_id == 0) {
      BOOST_LOG(warning) << "Restore hotkey thread not ready; update skipped.";
      return;
    }

    if (!PostThreadMessage(thread_id, kMsgUpdateHotkey, static_cast<WPARAM>(vk_code), static_cast<LPARAM>(modifiers))) {
      BOOST_LOG(warning) << "Failed to post restore hotkey update: "sv << GetLastError();
    }
  }
}  // namespace platf::hotkey
#endif

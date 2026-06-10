#include "src/platform/windows/display_helper_v2/win_platform_workarounds.h"

#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>
#include <display_device/windows/win_api_utils.h>
#include <display_device/windows/settings_utils.h>

#include <shlobj.h>
#include <windows.h>

#include <thread>

namespace display_helper::v2 {
  namespace {
    void refresh_shell_after_display_change() {
      SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
      SystemParametersInfoW(SPI_SETICONS, 0, nullptr, SPIF_SENDCHANGE);

      auto broadcast = [](UINT msg, WPARAM wParam, LPARAM lParam) {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(HWND_BROADCAST, msg, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 100, &result);
      };

      static const wchar_t kShellState[] = L"ShellState";
      static const wchar_t kIconMetrics[] = L"IconMetrics";
      broadcast(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kShellState));
      broadcast(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kIconMetrics));

      HDC hdc = GetDC(nullptr);
      int bpp = 32;
      if (hdc) {
        const int planes = GetDeviceCaps(hdc, PLANES);
        const int bits = GetDeviceCaps(hdc, BITSPIXEL);
        if (planes > 0 && bits > 0) {
          bpp = planes * bits;
        }
        ReleaseDC(nullptr, hdc);
      }
      const LPARAM res = MAKELPARAM(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
      broadcast(WM_DISPLAYCHANGE, static_cast<WPARAM>(bpp), res);
    }
  }  // namespace

  void WinPlatformWorkarounds::blank_hdr_states(std::chrono::milliseconds delay) {
    std::thread([delay]() {
      try {
        auto api = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice display(api);
        display_device::win_utils::blankHdrStates(display, delay);
      } catch (...) {
      }
    }).detach();
  }

  void WinPlatformWorkarounds::refresh_shell() {
    refresh_shell_after_display_change();
  }
}  // namespace display_helper::v2

#include "src/platform/windows/display_helper_v2/win_platform_workarounds.h"
#include "src/platform/windows/display_helper_shell_refresh_policy.h"

#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>
#include <display_device/windows/win_api_utils.h>
#include <display_device/windows/settings_utils.h>

#include <shlobj.h>
#include <windows.h>

#include <thread>

namespace display_helper::v2 {
  namespace {
    void broadcast_shell_change(UINT msg, WPARAM wParam, LPARAM lParam) {
      DWORD_PTR result = 0;
      SendMessageTimeoutW(HWND_BROADCAST, msg, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, 100, &result);
    }
  }  // namespace

  WinPlatformWorkarounds::WinPlatformWorkarounds():
      shell_broadcast_worker_([this](std::stop_token stop_token) {
        run_shell_broadcast_worker(stop_token);
      }),
      hdr_blank_worker_([this](std::stop_token stop_token) {
        run_hdr_blank_worker(stop_token);
      }) {}

  WinPlatformWorkarounds::~WinPlatformWorkarounds() {
    shell_broadcast_worker_.request_stop();
    hdr_blank_worker_.request_stop();
    shell_broadcast_cv_.notify_one();
    hdr_blank_cv_.notify_one();
    if (shell_broadcast_worker_.joinable()) {
      shell_broadcast_worker_.join();
    }
    if (hdr_blank_worker_.joinable()) {
      hdr_blank_worker_.join();
    }
  }

  void WinPlatformWorkarounds::blank_hdr_states(std::chrono::milliseconds delay) {
    {
      std::lock_guard lock(hdr_blank_mutex_);
      // Keep the workaround serialized and helper-owned, but never join the
      // prior one on the state-machine thread before publishing ApplyResult.
      // Multiple requests while the worker is busy coalesce to the latest
      // requested delay, matching v1's post-Apply execution boundary.
      hdr_blank_delay_ = delay;
      hdr_blank_pending_ = true;
    }
    hdr_blank_cv_.notify_one();
  }

  void WinPlatformWorkarounds::clear_pending_hdr_blank() {
    std::lock_guard lock(hdr_blank_mutex_);
    hdr_blank_pending_ = false;
  }

  void WinPlatformWorkarounds::run_hdr_blank_worker(std::stop_token stop_token) {
    std::unique_lock lock(hdr_blank_mutex_);
    while (!stop_token.stop_requested()) {
      hdr_blank_cv_.wait(lock, [this, stop_token] {
        return stop_token.stop_requested() || hdr_blank_pending_;
      });
      if (stop_token.stop_requested()) {
        break;
      }

      const auto delay = hdr_blank_delay_;
      hdr_blank_pending_ = false;
      lock.unlock();
      try {
        auto api = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice display(api);
        display_device::win_utils::blankHdrStates(display, delay);
      } catch (...) {
      }
      lock.lock();
    }
  }

  void WinPlatformWorkarounds::refresh_shell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    // SPIF_SENDCHANGE broadcasts synchronously without the timeout used by
    // the bounded messages below. A hung desktop application can therefore
    // hold the stream-start verification acknowledgment for many seconds.
    // Refresh the icon metrics locally, then use our bounded broadcasts.
    const UINT system_parameter_flags =
      shell_refresh_policy::allows_unbounded_system_parameter_broadcast() ? SPIF_SENDCHANGE : 0;
    SystemParametersInfoW(SPI_SETICONS, 0, nullptr, system_parameter_flags);

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
    if (shell_refresh_policy::defers_window_broadcasts_from_capture_gate()) {
      // WM_DISPLAYCHANGE contains no pointer parameters, so Windows can
      // deliver it asynchronously. Dispatch it before capture is released;
      // the pointer-bearing WM_SETTINGCHANGE notifications stay on the
      // coalescing worker below.
      SendNotifyMessageW(HWND_BROADCAST, WM_DISPLAYCHANGE, static_cast<WPARAM>(bpp), res);
      dispatch_window_broadcasts();
    } else {
      broadcast_shell_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ShellState"));
      broadcast_shell_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"IconMetrics"));
      broadcast_shell_change(WM_DISPLAYCHANGE, static_cast<WPARAM>(bpp), res);
    }
  }

  void WinPlatformWorkarounds::dispatch_window_broadcasts() {
    {
      std::lock_guard lock(shell_broadcast_mutex_);
      shell_broadcast_pending_ = true;
    }
    shell_broadcast_cv_.notify_one();
  }

  void WinPlatformWorkarounds::run_shell_broadcast_worker(std::stop_token stop_token) {
    static const wchar_t kShellState[] = L"ShellState";
    static const wchar_t kIconMetrics[] = L"IconMetrics";

    std::unique_lock lock(shell_broadcast_mutex_);
    while (!stop_token.stop_requested()) {
      shell_broadcast_cv_.wait(lock, [this, stop_token] {
        return stop_token.stop_requested() || shell_broadcast_pending_;
      });
      if (stop_token.stop_requested()) {
        break;
      }

      shell_broadcast_pending_ = false;
      lock.unlock();
      // SendMessageTimeout(HWND_BROADCAST, ...) waits up to its timeout for
      // every top-level window. Keep the legacy notifications, but a slow or
      // hung window must never delay the verification result that opens WGC.
      broadcast_shell_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kShellState));
      broadcast_shell_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kIconMetrics));
      lock.lock();
    }
  }
}  // namespace display_helper::v2

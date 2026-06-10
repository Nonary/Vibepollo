#pragma once

#include "src/platform/windows/display_helper_v2/types.h"

#include <atomic>
#include <functional>
#include <stop_token>
#include <thread>

#include <windows.h>

namespace display_helper::v2 {
  class WinEventPump {
  public:
    using Callback = std::function<void(DisplayEvent)>;

    void start(Callback callback);
    void stop();
    ~WinEventPump();

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void signal(DisplayEvent event);
    void cleanup_notifications();
    void thread_proc(std::stop_token st);

    Callback callback_;
    std::atomic<HWND> hwnd_ {nullptr};
    std::jthread worker_;
    HPOWERNOTIFY power_cookie_ = nullptr;
    HDEVNOTIFY device_cookie_ = nullptr;
  };
}  // namespace display_helper::v2

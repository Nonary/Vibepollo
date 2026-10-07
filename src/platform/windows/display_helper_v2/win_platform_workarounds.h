#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace display_helper::v2 {
  class WinPlatformWorkarounds final : public IPlatformWorkarounds {
  public:
    WinPlatformWorkarounds();
    ~WinPlatformWorkarounds() override;

    void blank_hdr_states(std::chrono::milliseconds delay) override;
    void clear_pending_hdr_blank() override;
    void refresh_shell() override;

  private:
    void dispatch_window_broadcasts();
    void run_shell_broadcast_worker(std::stop_token stop_token);
    void run_hdr_blank_worker(std::stop_token stop_token);

    std::mutex shell_broadcast_mutex_;
    std::condition_variable shell_broadcast_cv_;
    bool shell_broadcast_pending_ = false;
    std::jthread shell_broadcast_worker_;
    std::mutex hdr_blank_mutex_;
    std::condition_variable hdr_blank_cv_;
    bool hdr_blank_pending_ = false;
    std::chrono::milliseconds hdr_blank_delay_ {0};
    std::jthread hdr_blank_worker_;
  };
}  // namespace display_helper::v2

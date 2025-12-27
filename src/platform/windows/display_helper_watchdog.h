#pragma once

#include <chrono>
#include <functional>

namespace display_helper_integration {
  class DisplayHelperWatchdog {
  public:
    struct Hooks {
      std::function<bool()> feature_enabled;
      std::function<bool()> ensure_helper_started;
      std::function<bool()> send_ping;
      std::function<void()> reset_connection;
      std::function<int()> session_count;
      std::function<int()> running_processes;
    };

    explicit DisplayHelperWatchdog(Hooks hooks);

    std::chrono::milliseconds tick();
    void reset();
    bool helper_ready() const;

    static std::chrono::milliseconds active_interval();
    static std::chrono::milliseconds suspended_interval();

  private:
    Hooks hooks_;
    bool helper_ready_ = false;
  };
}  // namespace display_helper_integration

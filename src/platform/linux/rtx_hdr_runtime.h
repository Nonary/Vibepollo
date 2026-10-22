#pragma once
#include "truehdr.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace platf::rtx_hdr {
  struct live_output_metadata_state_t {
    std::uint32_t generation = 0;
    int peak_brightness = 1000;
  };

  struct frame_settings_t {
    std::uint32_t generation = 0;
    bool enabled = false;
    int sdr_brightness = 0;
    linux_truehdr::settings_t values;
  };

  // Publish only completed config applies. Encode threads never read mutable
  // config fields, take the config write gate, or perform foreground IPC.
  void notify_live_settings_changed();
  std::shared_ptr<const frame_settings_t> frame_settings();
  live_output_metadata_state_t live_output_metadata_state();

  class foreground_t {
  public:
    foreground_t();
    ~foreground_t();
    bool matches() const;

  private:
    void poll(std::stop_token stop);
    std::atomic<int> app_id {0};
    std::atomic<bool> matched {false};
    std::atomic<std::int64_t> observed_at {0};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread worker;
  };
}  // namespace platf::rtx_hdr

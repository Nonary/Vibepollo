/**
 * @file src/platform/windows/rtx_hdr_runtime.h
 * @brief Per-frame RTX HDR foreground/profile runtime state.
 */
#pragma once

#include "foreground_app.h"
#include "rtx_hdr_profile.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <winsock2.h>
#include <windows.h>

namespace platf::rtx_hdr {

  struct frame_state_t: runtime_values_t {
    bool has_active_app {false};
    bool foreground_matches {false};
    bool lookup_available {false};
    std::string foreground_exe;
    std::string active_app_exe;
    std::string foreground_source;
  };

  struct live_output_metadata_state_t {
    std::uint32_t generation {0};
    int peak_brightness {0};
  };

  void notify_live_settings_changed();
  std::uint64_t live_settings_generation();
  live_output_metadata_state_t live_output_metadata_state();
  float sdr_brightness_to_white_nits(int brightness);

#ifdef SUNSHINE_TESTS
  struct runtime_test_hooks_t {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<platf::foreground_app::state_t(const std::optional<RECT> &capture_rect)> foreground_snapshot;
    std::function<resolved_profile_t(const std::string &executable)> resolve_profile;
    bool start_background_threads {false};
  };
#endif

  class runtime_t {
  public:
    struct backend_t;
    struct shared_state_t;

    runtime_t();
#ifdef SUNSHINE_TESTS
    explicit runtime_t(runtime_test_hooks_t hooks);
#endif
    ~runtime_t();

    runtime_t(const runtime_t &) = delete;
    runtime_t &operator=(const runtime_t &) = delete;
    runtime_t(runtime_t &&) = delete;
    runtime_t &operator=(runtime_t &&) = delete;

    frame_state_t update_for_frame(const std::optional<RECT> &capture_rect);

#ifdef SUNSHINE_TESTS
    void poll_foreground_for_tests();
    bool run_pending_profile_lookup_for_tests();
    std::chrono::milliseconds profile_refresh_interval_for_tests() const;
#endif

  private:
    explicit runtime_t(backend_t backend);
    void start_workers();

    std::shared_ptr<shared_state_t> state;
    std::once_flag start_once;
    std::thread foreground_worker;
    std::thread profile_worker;
  };

}  // namespace platf::rtx_hdr

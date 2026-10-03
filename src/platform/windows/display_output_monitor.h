#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <windows.h>
#include <dxgi1_6.h>

namespace platf::dxgi {
  // Owns all output enumeration and event subscriptions. The capture thread
  // consumes only atomics; the monitor never modifies a capture D3D/DXGI object.
  class display_output_monitor_t {
  public:
    display_output_monitor_t(LUID adapter, DXGI_OUTPUT_DESC output, bool hdr_valid, bool hdr,
                             int offset_x, int offset_y, int env_width, int env_height, std::uint64_t mutation_revision);
    ~display_output_monitor_t();
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    bool reinit_requested() const { return reinit_.load(std::memory_order_acquire); }
    bool hold_frames() const;
    bool mutation_current() const;
    bool wait_for_initial_validation(std::chrono::milliseconds timeout);

  private:
    void run(std::stop_token stop);
    const LUID adapter_;
    const DXGI_OUTPUT_DESC output_;
    const bool hdr_valid_, hdr_;
    const int offset_x_, offset_y_, env_width_, env_height_;
    const std::uint64_t mutation_revision_;
    std::atomic_bool failed_ {false};
    std::atomic_bool reinit_ {false};
    std::atomic_bool hold_ {true};
    std::atomic_bool initial_validated_ {false};
    struct notifications_t {
      std::atomic_bool dirty {true};
      std::atomic_bool notified {false};
      std::atomic_int expected_hdr {-1};
      std::mutex mutex;
      std::condition_variable_any cv;
    };
    std::shared_ptr<notifications_t> notifications_ = std::make_shared<notifications_t>();
    std::jthread worker_;
  };
}

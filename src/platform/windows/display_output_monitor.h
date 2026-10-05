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
                             int offset_x, int offset_y, int env_width, int env_height);
    ~display_output_monitor_t();
    bool reinit_requested() const {
      return notifications_->reinit.load(std::memory_order_acquire);
    }

  private:
    void run(std::stop_token stop);
    const LUID adapter_;
    const DXGI_OUTPUT_DESC output_;
    const bool hdr_valid_, hdr_;
    const int offset_x_, offset_y_, env_width_, env_height_;
    struct notifications_t {
      std::atomic_bool dirty {true};
      std::atomic_bool reinit {false};
      std::mutex mutex;
      std::condition_variable_any cv;
    };
    std::shared_ptr<notifications_t> notifications_ = std::make_shared<notifications_t>();
    std::jthread worker_;
  };
}

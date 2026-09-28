/**
 * @file src/platform/windows/capture_output_validation.h
 * @brief Periodic display-state validation that stays off the frame-pacing thread.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace platf::dxgi::capture_policy {

  // WGC capture re-enumerates DXGI once per second to catch HDR transitions
  // that a stale factory can hide. Factory creation plus adapter/output
  // enumeration measured 0.6 ms median and 1.7 ms worst case on an idle AMD
  // desktop, and it can take longer while the display configuration changes.
  // On the capture thread that cost lands right before the pacing sleep, where
  // it can push a wake past its slot and re-anchor the frame grid once per
  // second. The probe runs here instead; the capture loop only reads the
  // latched result.
  class background_output_validator_t {
  public:
    // probe() returns true when the captured output changed structurally. The
    // first probe runs immediately. After a positive result the worker exits:
    // the capture session is about to be rebuilt with fresh state.
    template<class Probe>
    background_output_validator_t(std::chrono::steady_clock::duration period, Probe probe):
        _worker([this, period, probe = std::move(probe)](std::stop_token stop) mutable {
          run(stop, period, probe);
        }) {}

    background_output_validator_t(const background_output_validator_t &) = delete;
    background_output_validator_t &operator=(const background_output_validator_t &) = delete;

    ~background_output_validator_t() {
      _worker.request_stop();
      if (_worker.joinable()) {
        _worker.join();
      }
    }

    bool structural_change_detected() const noexcept {
      return _structural_change.load(std::memory_order_acquire);
    }

  private:
    template<class Probe>
    void run(std::stop_token stop, std::chrono::steady_clock::duration period, Probe &probe) {
      while (!stop.stop_requested()) {
        if (probe()) {
          _structural_change.store(true, std::memory_order_release);
          return;
        }

        std::unique_lock lock(_mutex);
        // Returns early on request_stop(), so teardown never waits out the period.
        _wakeup.wait_for(lock, stop, period, [] {
          return false;
        });
      }
    }

    std::atomic<bool> _structural_change {false};
    std::mutex _mutex;
    std::condition_variable_any _wakeup;
    // Declared last so every member above exists before the worker starts.
    std::jthread _worker;
  };

}  // namespace platf::dxgi::capture_policy

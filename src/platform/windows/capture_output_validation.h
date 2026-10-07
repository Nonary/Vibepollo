/**
 * @file src/platform/windows/capture_output_validation.h
 * @brief Display-state validation that stays off the frame-pacing thread.
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

  /**
   * Whether the validator should still probe on its period. Without change
   * notifications it always does; with them, only until the settle window
   * after the latest notification has passed.
   */
  constexpr bool keeps_polling(
    const bool notifications_available,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::steady_clock::time_point poll_until
  ) noexcept {
    return !notifications_available || now < poll_until;
  }

  // WGC capture re-enumerates DXGI to catch HDR transitions that a stale
  // factory can hide. Factory creation plus adapter/output enumeration
  // measured 0.6 ms median and 1.7 ms worst case on an idle AMD desktop, and
  // it can take longer while the display configuration changes. On the
  // capture thread that cost lands right before the pacing sleep, where it
  // can push a wake past its slot and re-anchor the frame grid once per
  // second. The probe runs here instead; the capture loop only reads the
  // latched result. When the system reports display color changes, the
  // validator probes only around those reports instead of once per period.
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

    // Change notifications are available: stop polling once `settle` passes
    // without one. Polling continues for `settle` from now, which covers a
    // change that raced the subscription.
    void rely_on_notifications(std::chrono::steady_clock::duration settle) {
      std::lock_guard lock(_mutex);
      _notifications_available = true;
      _settle = settle;
      _poll_until = std::chrono::steady_clock::now() + settle;
    }

    // A display change was reported. Probe now, then keep probing every
    // period for the settle window: DXGI can report the new state late.
    void notify() {
      {
        std::lock_guard lock(_mutex);
        _notified = true;
      }
      _wakeup.notify_all();
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
        const auto notified = [this] {
          return _notified;
        };
        // Both waits return early on request_stop(), so teardown never waits
        // out the period or the next notification.
        if (keeps_polling(_notifications_available, std::chrono::steady_clock::now(), _poll_until)) {
          _wakeup.wait_for(lock, stop, period, notified);
        } else {
          _wakeup.wait(lock, stop, notified);
        }
        if (_notified) {
          _notified = false;
          _poll_until = std::chrono::steady_clock::now() + _settle;
        }
      }
    }

    std::atomic<bool> _structural_change {false};
    std::mutex _mutex;
    std::condition_variable_any _wakeup;
    bool _notifications_available = false;  ///< Guarded by _mutex.
    bool _notified = false;  ///< Guarded by _mutex.
    std::chrono::steady_clock::duration _settle {};  ///< Guarded by _mutex.
    std::chrono::steady_clock::time_point _poll_until {};  ///< Guarded by _mutex.
    // Declared last so every member above exists before the worker starts.
    std::jthread _worker;
  };

}  // namespace platf::dxgi::capture_policy

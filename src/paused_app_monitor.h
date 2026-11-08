/**
 * @file src/paused_app_monitor.h
 * @brief Observe disconnected applications without extending their lifetime.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace proc {
  inline bool paused_app_poll_allowed(int app_id, bool launch_deferred, bool capture_owned) {
    return app_id > 0 && !launch_deferred && !capture_owned;
  }

  /**
   * Serialize observation with replacement/termination. Both the eligibility
   * check and poll belong to this critical section: a separate deferred-launch
   * check could otherwise observe one app and launch its replacement.
   * snapshot must be safe without the gate; callbacks must not reacquire it.
   */
  template<class Snapshot, class Eligible, class Poll>
  int observe_app_liveness(std::mutex &gate, Snapshot snapshot, Eligible eligible, Poll poll) {
    std::unique_lock lock(gate, std::try_to_lock);
    if (!lock.owns_lock() || !eligible()) {
      return snapshot();
    }
    return poll();
  }

  /**
   * One worker owned by process initialization, not by an individual session.
   * It can observe later paused apps without a stop/start race or stale session
   * callback. The callback must contain exceptions and decide eligibility under
   * the lifecycle gate. Shutdown joins before any process resources are freed.
   * stop() is called only by the owner, outside the lifecycle gate and callback.
   */
  class paused_app_monitor_t {
  public:
    paused_app_monitor_t(std::function<void()> poll, std::chrono::milliseconds interval):
        poll_(std::move(poll)),
        interval_(interval),
        worker_([this](std::stop_token stop) {
          std::unique_lock lock(mutex_);
          while (!stop.stop_requested()) {
            cv_.wait_for(lock, stop, interval_, [] { return false; });
            if (stop.stop_requested()) break;
            lock.unlock();
            poll_();
            lock.lock();
          }
        }) {
    }

    paused_app_monitor_t(const paused_app_monitor_t &) = delete;
    paused_app_monitor_t &operator=(const paused_app_monitor_t &) = delete;

    ~paused_app_monitor_t() {
      stop();
    }

    void stop() {
      worker_.request_stop();
      cv_.notify_all();
      if (worker_.joinable()) worker_.join();
    }

  private:
    std::function<void()> poll_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    // Construct last, destroy/join before the callback and wait state.
    std::jthread worker_;
  };
}  // namespace proc

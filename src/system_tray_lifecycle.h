/**
 * @file src/system_tray_lifecycle.h
 * @brief Owned, cancellable Windows tray worker lifecycle.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace system_tray::detail {
  class worker_t {
  public:
    worker_t() = default;
    worker_t(const worker_t &) = delete;
    worker_t &operator=(const worker_t &) = delete;

    ~worker_t() {
      stop();
    }

    template<class Fn>
    bool start(Fn &&fn) {
      std::lock_guard lock(mutex_);
      if (joining_ || thread_.joinable()) {
        return false;
      }
      stop_requested_ = false;
      thread_ = std::thread(std::forward<Fn>(fn));
      worker_id_ = thread_.get_id();
      return true;
    }

    bool stop_requested() const {
      return stop_requested_.load();
    }

    template<class Rep, class Period>
    bool wait_for_stop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock lock(mutex_);
      return cv_.wait_for(lock, delay, [this]() {
        return stop_requested();
      });
    }

    void stop() {
      std::unique_lock lock(mutex_);
      stop_requested_ = true;
      cv_.notify_all();

      // A tray callback may request its own shutdown while another caller is
      // joining it. Keep ownership with that caller; never self-join or detach.
      if (worker_id_ == std::this_thread::get_id()) {
        return;
      }
      if (joining_) {
        cv_.wait(lock, [this]() {
          return !joining_;
        });
        return;
      }
      if (!thread_.joinable()) {
        return;
      }

      joining_ = true;
      auto thread = std::move(thread_);
      lock.unlock();
      thread.join();
      lock.lock();
      worker_id_ = {};
      joining_ = false;
      cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_requested_ = false;
    std::thread thread_;
    std::thread::id worker_id_;
    bool joining_ = false;
  };
}  // namespace system_tray::detail

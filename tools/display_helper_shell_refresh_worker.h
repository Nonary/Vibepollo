#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace display_helper {
  // Serialize cosmetic shell notifications. Requests coalesce while one is in
  // flight; the next callback samples the current topology. Keep a pending
  // request through cancellation and drain it at shutdown so an older message
  // cannot be the final notification after a restore.
  class shell_refresh_worker {
  public:
    explicit shell_refresh_worker(std::function<void(std::stop_token)> refresh):
        refresh_(std::move(refresh)), thread_([this](std::stop_token stop) { run(stop); }) {}

    ~shell_refresh_worker() {
      {
        std::lock_guard lock(mutex_);
        closing_ = true;
      }
      cv_.notify_one();
      thread_.join();
    }

    void request() {
      {
        std::lock_guard lock(mutex_);
        pending_ = true;
      }
      cv_.notify_one();
    }

  private:
    void run(std::stop_token stop) {
      std::unique_lock lock(mutex_);
      for (;;) {
        cv_.wait(lock, [this] { return pending_ || closing_; });
        if (!pending_ && closing_) return;
        pending_ = false;
        lock.unlock();
        refresh_(stop);
        lock.lock();
      }
    }

    std::function<void(std::stop_token)> refresh_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ {false};
    bool closing_ {false};
    std::jthread thread_;
  };
}  // namespace display_helper

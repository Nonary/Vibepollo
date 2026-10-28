/** @file Serialized, cancellable scheduling of Linux display helper work. */
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace platf::linux_private_display {
  class restore_dispatcher_t {
  public:
    using task_t = std::function<void(std::stop_token)>;

    explicit restore_dispatcher_t(std::function<void()> report_failure = {}):
        report_failure_(std::move(report_failure)) {}
    ~restore_dispatcher_t() { stop(); }

    bool submit(std::uint64_t generation, task_t task,
                std::chrono::milliseconds delay = {}) {
      std::lock_guard lock {mutex_};
      if (stopping_ || generation <= last_generation_) return false;
      if (!worker_.joinable()) {
        try {
          worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        } catch (...) {
          return false;
        }
      }
      last_generation_ = generation;
      pending_ = request_t {generation, std::chrono::steady_clock::now() + delay, std::move(task)};
      cv_.notify_all();
      return true;
    }

    void cancel(std::uint64_t generation) {
      std::lock_guard lock {mutex_};
      last_generation_ = std::max(last_generation_, generation);
      if (pending_ && pending_->generation <= generation) pending_.reset();
      cv_.notify_all();
    }

    void drain() {
      std::unique_lock lock {mutex_};
      cv_.wait(lock, [&] { return !pending_ && !executing_; });
    }

    // Called by the owning host shutdown thread, after closing mutation admission.
    // In-flight helper transactions drain to their deadline before the owner dies.
    void stop() {
      {
        std::lock_guard lock {mutex_};
        stopping_ = true;
        pending_.reset();
        if (worker_.joinable()) worker_.request_stop();
      }
      cv_.notify_all();
      if (worker_.joinable()) worker_.join();
    }

  private:
    struct request_t {
      std::uint64_t generation;
      std::chrono::steady_clock::time_point due;
      task_t task;
    };

    void run(std::stop_token stop) {
      std::unique_lock lock {mutex_};
      while (!stop.stop_requested()) {
        cv_.wait(lock, [&] { return stop.stop_requested() || pending_.has_value(); });
        if (stop.stop_requested()) break;
        const auto generation = pending_->generation;
        const auto due = pending_->due;
        if (cv_.wait_until(lock, due, [&] {
              return stop.stop_requested() || !pending_ || pending_->generation != generation;
            })) continue;
        auto task = std::move(pending_->task);
        pending_.reset();
        executing_ = true;
        lock.unlock();
        try {
          task(stop);
        } catch (...) {
          try {
            if (report_failure_) report_failure_();
          } catch (...) {}
        }
        lock.lock();
        executing_ = false;
        cv_.notify_all();
      }
    }

    std::function<void()> report_failure_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<request_t> pending_;
    std::uint64_t last_generation_ {0};
    bool stopping_ {false};
    bool executing_ {false};
    std::jthread worker_;
  };
}  // namespace platf::linux_private_display

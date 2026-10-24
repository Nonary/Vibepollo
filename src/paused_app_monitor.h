/**
 * @file src/paused_app_monitor.h
 * @brief Keeps observing a paused app so its exit still ends the session.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace proc {
  /**
   * proc_t::running() reaps an exited app and restores the displays, but only
   * stream control threads and client requests call it. Once the last client
   * disconnects, an app that exits stays "paused" until some client next asks
   * for server status, which can be hours later. This worker keeps calling the
   * poll hook until the app is gone.
   */
  class paused_app_monitor_t {
  public:
    struct hooks_t {
      /// True while an app (running or paused) still owns the session.
      std::function<bool()> app_active;
      /// False skips one poll without ending the worker.
      std::function<bool()> should_poll;
      /// Observes the app and performs cleanup if it exited.
      std::function<void()> poll;
    };

    paused_app_monitor_t(hooks_t hooks, std::chrono::milliseconds interval):
        hooks_(std::move(hooks)),
        interval_(interval) {
    }

    paused_app_monitor_t(const paused_app_monitor_t &) = delete;
    paused_app_monitor_t &operator=(const paused_app_monitor_t &) = delete;

    ~paused_app_monitor_t() {
      stop();
    }

    /**
     * Starts the worker if it is not already watching. Safe to call while
     * holding the stream lifecycle gate: it only joins a worker that has
     * already left its loop.
     */
    void start() {
      std::lock_guard lock(mutex_);
      if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id() || !exited_) {
          return;
        }
        worker_.join();
      }
      exited_ = false;
      worker_ = std::jthread([this](std::stop_token st) {
        run(st);
      });
    }

    /**
     * Stops and joins the worker. From a poll hook this does nothing; the
     * worker ends once app_active() turns false.
     */
    void stop() {
      std::jthread worker;
      {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable() || worker_.get_id() == std::this_thread::get_id()) {
          return;
        }
        worker = std::move(worker_);
      }
      worker.request_stop();
      cv_.notify_all();
      worker.join();
    }

    bool watching() const {
      std::lock_guard lock(mutex_);
      return worker_.joinable() && !exited_;
    }

  private:
    void run(std::stop_token st) {
      while (true) {
        {
          std::unique_lock lock(mutex_);
          cv_.wait_for(lock, st, interval_, [] {
            return false;
          });
          // Decide to exit under the lock so a concurrent start() either sees
          // this worker still watching or joins it and starts a fresh one.
          if (st.stop_requested() || !hooks_.app_active()) {
            exited_ = true;
            return;
          }
        }
        if (hooks_.should_poll()) {
          hooks_.poll();
        }
      }
    }

    hooks_t hooks_;
    std::chrono::milliseconds interval_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::jthread worker_;
    bool exited_ {false};
  };
}  // namespace proc

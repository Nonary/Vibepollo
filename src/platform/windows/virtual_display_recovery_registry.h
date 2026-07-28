#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace vdisplay_recovery {

  // Owns recovery workers until a non-worker thread has joined them. This is
  // deliberately separate from the driver's operation lock: cancellation may
  // be requested while a removal owns that lock, but joining happens without
  // the registry or shutdown-publication locks held.
  class monitor_registry_t {
  public:
    using worker_t = std::function<void(std::stop_token)>;

    monitor_registry_t() = default;
    monitor_registry_t(const monitor_registry_t &) = delete;
    monitor_registry_t &operator=(const monitor_registry_t &) = delete;

    ~monitor_registry_t() {
      request_shutdown();
      join_all();
    }

    [[nodiscard]] bool shutdown_requested() const noexcept {
      return shutdown_requested_.load(std::memory_order_acquire);
    }

    // Replaces a same-key monitor only after the previous worker has stopped
    // and been joined. The transition marker lets other callers make progress
    // while that join occurs outside every registry lock. Publication of a
    // new worker is serialized with the shutdown latch.
    bool start(std::string key, worker_t worker) {
      for (;;) {
        {
          std::unique_lock lock(mutex_);
          cv_.wait(lock, [&] {
            return shutdown_requested() || transitions_.find(key) == transitions_.end();
          });
          if (shutdown_requested()) {
            return false;
          }
        }

        std::jthread worker_to_join;
        {
          // A shutdown request takes this same gate before publishing its
          // latch and stop requests. Thus it cannot return while a worker is
          // still being made visible without its stop token requested.
          std::lock_guard scheduling_lock(scheduling_mutex_);
          if (shutdown_requested()) {
            return false;
          }

          std::lock_guard lock(mutex_);
          if (transitions_.find(key) != transitions_.end()) {
            continue;
          }

          const auto existing = entries_.find(key);
          if (existing != entries_.end()) {
            // A recovery callback must not replace its own worker. It can
            // request cancellation, but only an external lifecycle owner can
            // join that worker.
            if (existing->second->worker.joinable() && existing->second->worker.get_id() == std::this_thread::get_id()) {
              existing->second->worker.request_stop();
              return false;
            }
            existing->second->worker.request_stop();
            worker_to_join = std::move(existing->second->worker);
            entries_.erase(existing);
            transitions_.insert(key);
            cv_.notify_all();
          } else {
            std::shared_ptr<entry_t> entry;
            try {
              entry = std::make_shared<entry_t>();
            } catch (...) {
              return false;
            }
            if (!ensure_reaper_locked()) {
              return false;
            }

            const auto [it, inserted] = entries_.emplace(key, entry);
            if (!inserted) {
              continue;
            }
            try {
              entry->worker = std::jthread([
                                             this,
                                             key,
                                           entry,
                                           worker = std::move(worker)
                                           ](std::stop_token stop_token) mutable {
                try {
                  worker(stop_token);
                } catch (...) {
                  // Recovery is best-effort. The registry must still mark this
                  // worker complete so an external reaper can join it safely.
                }
                mark_finished(key, entry);
              });
            } catch (...) {
              entries_.erase(it);
              cv_.notify_all();
              return false;
            }
            return true;
          }
        }

        if (worker_to_join.joinable()) {
          worker_to_join.join();
        }
        finish_transition(key);
        // Check shutdown again before creating a replacement.
      }
    }

    void request_stop(const std::string &key) {
      std::lock_guard lock(mutex_);
      if (const auto it = entries_.find(key); it != entries_.end()) {
        it->second->worker.request_stop();
      }
      cv_.notify_all();
    }

    void request_stop_all() {
      std::lock_guard lock(mutex_);
      for (auto &[_, entry] : entries_) {
        entry->worker.request_stop();
      }
      cv_.notify_all();
    }

    void request_shutdown() {
      // This lock is intentionally narrow. It serializes only shutdown
      // publication against a new worker becoming visible; joins stay outside
      // it so a worker can always finish driver-facing work.
      std::lock_guard scheduling_lock(scheduling_mutex_);
      shutdown_requested_.store(true, std::memory_order_release);
      request_stop_all();
    }

    // Called by the external lifecycle owner (main shutdown). A worker that
    // reaches this defensively requests global cancellation but leaves thread
    // ownership intact for a later external call; it never self-joins.
    void join_all() {
      request_shutdown();
      if (called_from_owned_thread()) {
        return;
      }

      std::jthread reaper_to_join;
      {
        std::lock_guard lock(mutex_);
        if (reaper_.joinable()) {
          reaper_.request_stop();
          reaper_to_join = std::move(reaper_);
        }
        cv_.notify_all();
      }
      if (reaper_to_join.joinable()) {
        reaper_to_join.join();
      }

      // A simultaneous start may already be joining a superseded worker when
      // shutdown begins. It observes the latch before adding a replacement.
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
          return transitions_.empty();
        });
      }

      for (;;) {
        std::jthread worker_to_join;
        {
          std::lock_guard lock(mutex_);
          if (entries_.empty()) {
            break;
          }
          auto it = entries_.begin();
          it->second->worker.request_stop();
          worker_to_join = std::move(it->second->worker);
          entries_.erase(it);
        }
        if (worker_to_join.joinable()) {
          worker_to_join.join();
        }
      }
    }

  private:
    struct entry_t {
      std::jthread worker;
      bool finished = false;
    };

    bool ensure_reaper_locked() {
      if (reaper_.joinable()) {
        return true;
      }
      try {
        reaper_ = std::jthread([this](std::stop_token stop_token) {
          reap_finished(stop_token);
        });
      } catch (...) {
        return false;
      }
      return true;
    }

    bool called_from_owned_thread() const {
      const auto caller = std::this_thread::get_id();
      std::lock_guard lock(mutex_);
      if (reaper_.joinable() && reaper_.get_id() == caller) {
        return true;
      }
      for (const auto &[_, entry] : entries_) {
        if (entry->worker.joinable() && entry->worker.get_id() == caller) {
          return true;
        }
      }
      return false;
    }

    void finish_transition(const std::string &key) {
      std::lock_guard lock(mutex_);
      transitions_.erase(key);
      cv_.notify_all();
    }

    void mark_finished(const std::string &key, const std::shared_ptr<entry_t> &entry) {
      std::lock_guard lock(mutex_);
      const auto it = entries_.find(key);
      if (it != entries_.end() && it->second == entry) {
        entry->finished = true;
      }
      cv_.notify_all();
    }

    void reap_finished(std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        std::jthread worker_to_join;
        std::string key;
        {
          std::unique_lock lock(mutex_);
          cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
            if (stop_token.stop_requested()) {
              return true;
            }
            for (const auto &[_, entry] : entries_) {
              if (entry->finished) {
                return true;
              }
            }
            return false;
          });
          if (stop_token.stop_requested()) {
            return;
          }

          auto it = entries_.end();
          for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
            if (candidate->second->finished) {
              it = candidate;
              break;
            }
          }
          if (it == entries_.end()) {
            continue;
          }
          key = it->first;
          worker_to_join = std::move(it->second->worker);
          entries_.erase(it);
          transitions_.insert(key);
          cv_.notify_all();
        }

        // finished is set as the worker's final operation, so this join is
        // immediate and cannot contend with driver or display-helper locks.
        if (worker_to_join.joinable()) {
          worker_to_join.join();
        }
        finish_transition(key);
      }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<entry_t>, std::less<>> entries_;
    std::set<std::string, std::less<>> transitions_;
    std::mutex scheduling_mutex_;
    std::jthread reaper_;
    std::atomic_bool shutdown_requested_ {false};
  };
}  // namespace vdisplay_recovery

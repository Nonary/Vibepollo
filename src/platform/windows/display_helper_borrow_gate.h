#pragma once

#include "src/platform/windows/display_helper_borrow_protocol.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace display_helper::borrow {
  // Shared by the real dispatcher and all direct backend mutation boundaries.
  // Closing admission parks pending work; it does not invoke a failure callback
  // or discard an existing apply/restore/watchdog transaction.
  class gate_t {
  public:
    class scope_t {
    public:
      scope_t() = default;
      scope_t(const scope_t &) = delete;
      scope_t &operator=(const scope_t &) = delete;
      scope_t(scope_t &&other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
      scope_t &operator=(scope_t &&other) noexcept {
        if (this != &other) { reset(); owner_ = std::exchange(other.owner_, nullptr); }
        return *this;
      }
      ~scope_t() { reset(); }
      explicit operator bool() const { return owner_ != nullptr; }
      void reset() { if (auto *owner = std::exchange(owner_, nullptr)) owner->leave(); }
    private:
      friend class gate_t;
      explicit scope_t(gate_t *owner) : owner_(owner) {}
      gate_t *owner_ = nullptr;
    };

    scope_t try_enter() {
      std::lock_guard lock(mutex_);
      if (depth_[this] != 0) { ++depth_[this]; return scope_t(this); }
      if (inhibited_locked()) return {};
      ++active_; depth_[this] = 1;
      return scope_t(this);
    }

    scope_t wait_enter(const std::function<bool()> &cancelled = {}) {
      std::unique_lock lock(mutex_);
      if (depth_[this] != 0) { ++depth_[this]; return scope_t(this); }
      while (inhibited_locked()) {
        if (cancelled && cancelled()) return {};
        cv_.wait_for(lock, std::chrono::milliseconds(10));
      }
      if (cancelled && cancelled()) return {};
      ++active_; depth_[this] = 1;
      return scope_t(this);
    }

    bool acquire(const request_t &request, std::chrono::steady_clock::time_point deadline,
                 const std::function<bool()> &cancelled = {}) {
      if (!request.valid()) return false;
      std::unique_lock lock(mutex_);
      if (permanent_ || request.id <= retired_[request.controller]) return false;
      held_.insert(request);
      cv_.notify_all();
      while (active_ != 0) {
        if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
          held_.erase(request);
          retired_[request.controller] = std::max(retired_[request.controller], request.id);
          cv_.notify_all();
          return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(10));
      }
      if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
        held_.erase(request);
        retired_[request.controller] = std::max(retired_[request.controller], request.id);
        cv_.notify_all();
        return false;
      }
      return true;
    }

    bool release(const request_t &request) {
      std::lock_guard lock(mutex_);
      // A late acquire cannot resurrect an already cancelled exact request.
      // IDs from other process-creation contexts have independent tombstones.
      retired_[request.controller] = std::max(retired_[request.controller], request.id);
      const bool found = held_.erase(request) != 0;
      cv_.notify_all();
      return found;
    }

    bool inhibited() const { std::lock_guard lock(mutex_); return inhibited_locked(); }
    bool permanently_inhibited() const { std::lock_guard lock(mutex_); return permanent_; }
    bool entered_on_this_thread() const { return depth_[this] != 0; }
    std::size_t active() const { std::lock_guard lock(mutex_); return active_; }
    void retain_unknown() { std::lock_guard lock(mutex_); permanent_ = true; cv_.notify_all(); }

  private:
    bool inhibited_locked() const { return permanent_ || !held_.empty(); }
    void leave() {
      std::lock_guard lock(mutex_);
      auto it = depth_.find(this);
      if (it != depth_.end() && --it->second == 0) { depth_.erase(it); --active_; cv_.notify_all(); }
    }
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::set<request_t> held_;
    std::map<process_context_t, std::uint64_t> retired_;
    std::size_t active_ = 0;
    bool permanent_ = false;
    inline static thread_local std::map<const gate_t *, std::size_t> depth_;
  };
  inline gate_t &process_gate() { static gate_t value; return value; }
}

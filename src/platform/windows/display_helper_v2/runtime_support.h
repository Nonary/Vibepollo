#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace display_helper::v2 {
  class CancellationToken {
  public:
    CancellationToken() = default;

    bool is_cancelled() const {
      if (!generation_) {
        return true;
      }
      return generation_->load(std::memory_order_acquire) != expected_generation_;
    }

    std::uint64_t generation() const {
      return expected_generation_;
    }

  private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<std::atomic<std::uint64_t>> generation, std::uint64_t expected)
      : generation_(std::move(generation)),
        expected_generation_(expected) {}

    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
    std::uint64_t expected_generation_ = 0;
  };

  class CancellationSource {
  public:
    CancellationSource()
      : generation_(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

    CancellationToken token() const {
      return CancellationToken(generation_, generation_->load(std::memory_order_acquire));
    }

    std::uint64_t cancel() {
      return generation_->fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    std::uint64_t current_generation() const {
      return generation_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
  };

  template <typename T>
  class MessageQueue {
  public:
    void push(const T &value) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(value);
      }
      cv_.notify_one();
    }

    void push(T &&value) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(value));
      }
      cv_.notify_one();
    }

    std::optional<T> try_pop() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) {
        return std::nullopt;
      }
      T value = std::move(queue_.front());
      queue_.pop_front();
      return value;
    }

    T wait_pop() {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&]() { return !queue_.empty(); });
      T value = std::move(queue_.front());
      queue_.pop_front();
      return value;
    }

    template <typename Rep, typename Period>
    std::optional<T> wait_for(const std::chrono::duration<Rep, Period> &timeout) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!cv_.wait_for(lock, timeout, [&]() { return !queue_.empty(); })) {
        return std::nullopt;
      }
      T value = std::move(queue_.front());
      queue_.pop_front();
      return value;
    }

    std::size_t size() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return queue_.size();
    }

    void clear() {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
  };

  class DebouncedTrigger {
  public:
    explicit DebouncedTrigger(std::chrono::milliseconds delay)
      : delay_(delay) {}

    void notify(std::chrono::steady_clock::time_point now) {
      pending_ = true;
      deadline_ = now + delay_;
    }

    bool should_fire(std::chrono::steady_clock::time_point now) {
      if (!pending_) {
        return false;
      }
      if (now < deadline_) {
        return false;
      }
      pending_ = false;
      return true;
    }

    bool pending() const {
      return pending_;
    }

    void reset() {
      pending_ = false;
    }

  private:
    std::chrono::milliseconds delay_;
    bool pending_ = false;
    std::chrono::steady_clock::time_point deadline_ {};
  };

  class DisconnectGrace {
  public:
    DisconnectGrace(IClock &clock, std::chrono::milliseconds grace)
      : clock_(clock),
        grace_(grace) {}

    void on_disconnect() {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = true;
      triggered_ = false;
      disconnect_at_ = clock_.now();
    }

    void on_reconnect() {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = false;
      triggered_ = false;
    }

    bool should_trigger() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_ || triggered_) {
        return false;
      }
      if (clock_.now() - disconnect_at_ >= grace_) {
        triggered_ = true;
        return true;
      }
      return false;
    }

  private:
    IClock &clock_;
    std::chrono::milliseconds grace_;
    std::chrono::steady_clock::time_point disconnect_at_ {};
    bool pending_ = false;
    bool triggered_ = false;
    std::mutex mutex_;
  };

  class ReconnectController {
  public:
    ReconnectController(IClock &clock, std::chrono::milliseconds grace)
      : grace_(clock, grace) {}

    void on_broken() {
      restart_pipe_ = true;
      was_connected_ = false;
      grace_.on_disconnect();
    }

    void on_error() {
      on_broken();
    }

    bool update_connection(bool connected) {
      if (connected && !was_connected_) {
        grace_.on_reconnect();
      } else if (!connected && was_connected_) {
        grace_.on_disconnect();
      }
      was_connected_ = connected;
      if (!connected && grace_.should_trigger()) {
        return true;
      }
      return false;
    }

    bool should_restart_pipe() const {
      return restart_pipe_;
    }

  private:
    DisconnectGrace grace_;
    bool was_connected_ = false;
    bool restart_pipe_ = false;
  };

  class HeartbeatMonitor {
  public:
    explicit HeartbeatMonitor(IClock &clock)
      : clock_(clock) {}

    void arm() {
      armed_ = true;
      timed_out_ = false;
      last_ping_ = clock_.now();
    }

    void disarm() {
      armed_ = false;
      timed_out_ = false;
    }

    void record_ping() {
      last_ping_ = clock_.now();
      timed_out_ = false;
    }

    bool check_timeout() {
      if (!armed_ || timed_out_) {
        return false;
      }
      if (clock_.now() - last_ping_ >= timeout_) {
        timed_out_ = true;
        return true;
      }
      return false;
    }

  private:
    IClock &clock_;
    bool armed_ = false;
    bool timed_out_ = false;
    std::chrono::steady_clock::time_point last_ping_ {};
    std::chrono::milliseconds timeout_ {std::chrono::seconds(30)};
  };

  class SystemClock final : public IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return std::chrono::steady_clock::now();
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      std::this_thread::sleep_for(duration);
    }
  };
}  // namespace display_helper::v2

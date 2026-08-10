#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <array>
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

    /// Remove and return queued entries selected by predicate while preserving
    /// their relative order. The helper uses this to give replacement control
    /// intents priority over an asynchronous completion from the mutation they
    /// supersede, without giving unrelated timer/completion traffic a second
    /// queue or another worker thread.
    template <typename Predicate>
    std::deque<T> extract_if(Predicate predicate) {
      std::deque<T> extracted;
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (predicate(*it)) {
          extracted.emplace_back(std::move(*it));
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
      return extracted;
    }

    /// Remove the matching prefix only. Unlike extract_if(), this preserves a
    /// non-control ingress frame (for example SNAPSHOT_CURRENT) as an ordering
    /// barrier before later replacement commands on the same pipe.
    template <typename Predicate>
    std::deque<T> extract_prefix_while(Predicate predicate) {
      std::deque<T> extracted;
      std::lock_guard<std::mutex> lock(mutex_);
      while (!queue_.empty() && predicate(queue_.front())) {
        extracted.emplace_back(std::move(queue_.front()));
        queue_.pop_front();
      }
      return extracted;
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

    void notify(std::chrono::steady_clock::time_point now, std::uint64_t generation = 0) {
      pending_ = true;
      deadline_ = now + delay_;
      generation_ = generation;
    }

    std::optional<std::uint64_t> take_if_due(std::chrono::steady_clock::time_point now) {
      if (!pending_) {
        return std::nullopt;
      }
      if (now < deadline_) {
        return std::nullopt;
      }
      pending_ = false;
      return generation_;
    }

    bool should_fire(std::chrono::steady_clock::time_point now) {
      return take_if_due(now).has_value();
    }

    bool pending() const {
      return pending_;
    }

    void reset() {
      pending_ = false;
      generation_ = 0;
    }

  private:
    std::chrono::milliseconds delay_;
    bool pending_ = false;
    std::chrono::steady_clock::time_point deadline_ {};
    std::uint64_t generation_ = 0;
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

  /**
   * @brief Schedules restore attempts the way the legacy helper's polling loop did:
   *        a 2-minute primary window after a REVERT, 30-second event windows after
   *        display events, a progressive backoff profile between failed attempts,
   *        and "window exhausted" detection that pauses attempts until the next event.
   */
  class RestoreScheduler {
  public:
    static constexpr auto kRestoreWindowPrimary = std::chrono::minutes(2);
    static constexpr auto kRestoreWindowEvent = std::chrono::seconds(30);
    static constexpr std::array<std::chrono::seconds, 8> kRestoreBackoffProfile {
      std::chrono::seconds(0),
      std::chrono::seconds(1),
      std::chrono::seconds(3),
      std::chrono::seconds(5),
      std::chrono::seconds(10),
      std::chrono::seconds(15),
      std::chrono::seconds(20),
      std::chrono::seconds(30)
    };

    void arm_primary(std::chrono::steady_clock::time_point now, std::chrono::milliseconds grace = std::chrono::milliseconds(0)) {
      armed_ = true;
      window_until_ = now + kRestoreWindowPrimary;
      next_allowed_ = now + grace;
      backoff_index_ = 0;
      window_exhausted_reported_ = false;
    }

    void on_display_event(std::chrono::steady_clock::time_point now) {
      if (!armed_) {
        return;
      }
      backoff_index_ = 0;
      next_allowed_ = now;
      const auto desired = now + kRestoreWindowEvent;
      if (desired > window_until_) {
        window_until_ = desired;
      }
      window_exhausted_reported_ = false;
    }

    void on_attempt_failed(std::chrono::steady_clock::time_point now) {
      if (backoff_index_ + 1 < kRestoreBackoffProfile.size()) {
        ++backoff_index_;
      }
      next_allowed_ = now + kRestoreBackoffProfile[backoff_index_];
    }

    void disarm() {
      armed_ = false;
      backoff_index_ = 0;
      window_exhausted_reported_ = false;
    }

    bool armed() const {
      return armed_;
    }

    bool window_active(std::chrono::steady_clock::time_point now) const {
      return armed_ && now <= window_until_;
    }

    /// True exactly once when the active window has just expired (pauses attempts
    /// until the next display event re-opens an event window).
    bool window_just_expired(std::chrono::steady_clock::time_point now) {
      if (!armed_ || window_exhausted_reported_) {
        return false;
      }
      if (now > window_until_) {
        window_exhausted_reported_ = true;
        return true;
      }
      return false;
    }

    bool should_attempt(std::chrono::steady_clock::time_point now) const {
      return window_active(now) && now >= next_allowed_;
    }

  private:
    bool armed_ = false;
    bool window_exhausted_reported_ = false;
    std::chrono::steady_clock::time_point window_until_ {};
    std::chrono::steady_clock::time_point next_allowed_ {};
    std::size_t backoff_index_ = 0;
  };

  class HeartbeatMonitor {
  public:
    explicit HeartbeatMonitor(IClock &clock)
      : clock_(clock) {}

    void arm() {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = true;
      const auto now = clock_.now();
      last_ping_ = now;
      optional_until_ = now + optional_window_;
      recovery_deadline_.reset();
    }

    void disarm() {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
      optional_until_ = {};
      recovery_deadline_.reset();
    }

    void record_ping() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!armed_) {
        return;
      }
      last_ping_ = clock_.now();
      // A reconnect during the v1 recovery grace window is healthy again; do
      // not allow an old missed-ping deadline to restore a live session.
      recovery_deadline_.reset();
    }

    bool check_timeout() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!armed_) {
        return false;
      }
      const auto now = clock_.now();
      if (now < optional_until_) {
        return false;
      }
      if (!recovery_deadline_) {
        if (now - last_ping_ < miss_window_) {
          return false;
        }
        recovery_deadline_ = now + recovery_window_;
        return false;
      }
      if (now >= *recovery_deadline_) {
        armed_ = false;
        optional_until_ = {};
        recovery_deadline_.reset();
        return true;
      }
      return false;
    }

  private:
    IClock &clock_;
    bool armed_ = false;
    std::chrono::steady_clock::time_point last_ping_ {};
    std::chrono::steady_clock::time_point optional_until_ {};
    std::optional<std::chrono::steady_clock::time_point> recovery_deadline_;
    std::chrono::milliseconds optional_window_ {std::chrono::seconds(30)};
    std::chrono::milliseconds miss_window_ {std::chrono::seconds(30)};
    std::chrono::milliseconds recovery_window_ {std::chrono::minutes(2)};
    std::mutex mutex_;
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

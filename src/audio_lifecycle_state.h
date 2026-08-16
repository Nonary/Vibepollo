/**
 * @file src/audio_lifecycle_state.h
 * @brief Synchronized ownership state for shared audio lifecycle transitions.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace audio::lifecycle {
  /**
   * @brief Synchronizes retained ownership, terminal restore, and successor handoff.
   *
   * The state is deliberately independent of Windows audio types so the
   * ownership transitions can be tested with a deterministic token. Production
   * code stores its audio control and captured sink snapshot in the same state.
   */
  template<class T>
  class state_t {
  public:
    bool retain(T &value, const bool app_active) {
      std::scoped_lock lock {_mutex};
      if (!app_active || _terminal_pending || _retained) {
        return false;
      }

      _retained = std::move(value);
      return true;
    }

    bool reclaim(T &value, const bool app_active) {
      wait_for_restore();

      std::scoped_lock lock {_mutex};
      if (!app_active || _terminal_pending || !_retained) {
        return false;
      }

      value = std::move(*_retained);
      _retained.reset();
      return true;
    }

    /**
     * @brief Reclaim retained ownership with an observer for a blocked wait.
     *
     * The observer is called only when a terminal restore is in progress and
     * while the state lock is held, immediately before waiting for completion.
     * Production callers use the overload above and pay no observer cost.
     */
    template<typename WaitObserver>
    bool reclaim(T &value, const bool app_active, WaitObserver &&observer) {
      wait_for_restore(std::forward<WaitObserver>(observer));

      std::scoped_lock lock {_mutex};
      if (!app_active || _terminal_pending || !_retained) {
        return false;
      }

      value = std::move(*_retained);
      _retained.reset();
      return true;
    }

    /**
     * @brief Atomically enter terminal state and claim retained ownership.
     * @return The retained value, if this call owns terminal restoration.
     */
    std::optional<T> begin_terminal() {
      std::scoped_lock lock {_mutex};
      _terminal_pending = true;
      _handoff_pending = false;

      if (_restore_in_progress || !_retained) {
        return std::nullopt;
      }

      _restore_in_progress = true;
      std::optional<T> value {std::move(*_retained)};
      _retained.reset();
      return value;
    }

    /** @brief Publish completion of the out-of-lock terminal restore. */
    void complete_terminal_restore() {
      {
        std::scoped_lock lock {_mutex};
        _restore_in_progress = false;
      }
      _restore_complete.notify_all();
    }

    /** @brief Block creation or reclaim until terminal restore is complete. */
    void wait_for_restore() {
      std::unique_lock lock {_mutex};
      _restore_complete.wait(lock, [this]() {
        return !_restore_in_progress;
      });
    }

    /**
     * @brief Wait for terminal restore while reporting entry into the wait.
     *
     * This generic overload is a deterministic test seam; the observer runs
     * only after observing the restore flag under the state lock.
     */
    template<typename WaitObserver>
    void wait_for_restore(WaitObserver &&observer) {
      std::unique_lock lock {_mutex};
      if (!_restore_in_progress) {
        return;
      }

      std::forward<WaitObserver>(observer)();
      _restore_complete.wait(lock, [this]() {
        return !_restore_in_progress;
      });
    }

    /**
     * @brief Begin a successor handoff without ending continuous ownership.
     *
     * The terminal marker remains set until the successor obtains an audio
     * owner. If no successor owner arrives, the old final owner still restores.
     */
    void app_started() {
      wait_for_restore();
      std::scoped_lock lock {_mutex};
      _handoff_pending = true;
    }

    /** @brief Publish that a capture has acquired shared audio ownership. */
    void owner_acquired() {
      std::scoped_lock lock {_mutex};
      ++_owner_count;
      if (_handoff_pending) {
        _terminal_pending = false;
        _handoff_pending = false;
      }
    }

    /** @brief Publish that a capture has released shared audio ownership. */
    void owner_released() {
      std::scoped_lock lock {_mutex};
      if (_owner_count != 0) {
        --_owner_count;
      }
    }

    [[nodiscard]] bool terminal_pending() const {
      std::scoped_lock lock {_mutex};
      return _terminal_pending;
    }

    [[nodiscard]] std::size_t owner_count() const {
      std::scoped_lock lock {_mutex};
      return _owner_count;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _restore_complete;
    std::optional<T> _retained;
    bool _terminal_pending {false};
    bool _handoff_pending {false};
    bool _restore_in_progress {false};
    std::size_t _owner_count {0};
  };
}  // namespace audio::lifecycle

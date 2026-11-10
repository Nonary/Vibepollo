/**
 * @file src/platform/windows/failed_revert_policy.h
 * @brief Remember a REVERT the display helper never received and decide when to replay it.
 */
#pragma once

#include <mutex>
#include <optional>
#include <utility>

namespace display_helper::failed_revert_policy {
  // The arguments of a REVERT whose dispatch failed. The replay must restore
  // with the same golden preference, ownership and display-removal policy.
  struct request_t {
    bool prefer_golden_if_current_missing = true;
    bool override_managed_ownership = false;
    bool allow_disabled_recovery = false;
    bool retain_paused_app = false;
    bool remove_display_after_restore = false;
    // Whether an app was running (paused for /resume) when the REVERT was issued.
    bool issued_with_app_running = false;
  };

  struct host_state_t {
    bool stream_active_or_pending = false;
    bool launch_pending = false;
    bool app_running = false;
  };

  // A replay must not race a stream or launch that will APPLY its own layout.
  // A running app blocks it too, unless an app was already running (paused)
  // when the missed REVERT was issued.
  inline bool may_replay(const request_t &request, const host_state_t &host) {
    if (host.stream_active_or_pending || host.launch_pending) {
      return false;
    }
    return !host.app_running || request.issued_with_app_running;
  }

  // Holds at most one missed REVERT. A later failure replaces it. Only an APPLY
  // that succeeds clears it, because the new layout's owner then restores it
  // when its session ends. An APPLY that fails, is skipped or is cancelled
  // leaves it pending with its original arguments, and none is replayed while
  // an APPLY is in flight.
  class slot_t {
  public:
    void remember(const request_t &request) {
      std::lock_guard lock(mutex_);
      request_ = request;
    }

    void clear() {
      std::lock_guard lock(mutex_);
      request_.reset();
    }

    void begin_apply() {
      std::lock_guard lock(mutex_);
      ++applies_in_flight_;
    }

    void finish_apply(const bool applied) {
      std::lock_guard lock(mutex_);
      if (applies_in_flight_ > 0) {
        --applies_in_flight_;
      }
      if (applied) {
        request_.reset();
      }
    }

    bool pending() const {
      std::lock_guard lock(mutex_);
      return request_.has_value();
    }

    // Removes and returns the missed REVERT if the host state allows replaying it.
    std::optional<request_t> take_if_replayable(const host_state_t &host) {
      std::lock_guard lock(mutex_);
      if (!request_ || applies_in_flight_ != 0 || !may_replay(*request_, host)) {
        return std::nullopt;
      }
      return std::exchange(request_, std::nullopt);
    }

  private:
    mutable std::mutex mutex_;
    std::optional<request_t> request_;
    int applies_in_flight_ = 0;
  };

  // Brackets one APPLY attempt. It settles as failed unless `applied(true)` is
  // called, including when the attempt returns early or throws.
  class apply_scope_t {
  public:
    explicit apply_scope_t(slot_t &slot):
        slot_(slot) {
      slot_.begin_apply();
    }

    apply_scope_t(const apply_scope_t &) = delete;
    apply_scope_t &operator=(const apply_scope_t &) = delete;

    ~apply_scope_t() {
      slot_.finish_apply(applied_);
    }

    void applied(const bool applied) {
      applied_ = applied;
    }

  private:
    slot_t &slot_;
    bool applied_ = false;
  };
}  // namespace display_helper::failed_revert_policy

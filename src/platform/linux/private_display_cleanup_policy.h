/**
 * @file src/platform/linux/private_display_cleanup_policy.h
 * @brief Serialize delayed display retirement with new stream ownership.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>

namespace platf::linux_private_display::cleanup_policy {
  struct no_completion_t {
    void operator()(std::uint64_t) const {}
  };

  enum class result_e {
    superseded,
    owned,
    restored,
    failed,
  };

  enum class admission_e {
    respect_owners,
    override_owners,
  };

  struct terminal_result_t {
    bool topology_restored {false};
    bool virtual_displays_removed {false};
  };

  /** Explicit termination still retires outputs when no physical restore guard exists. */
  template <typename Outputs, typename Restore, typename Disconnect, typename Verify, typename Allowed>
  terminal_result_t terminate_outputs(const Outputs &outputs, Restore restore, Disconnect disconnect, Verify verify, Allowed allowed) {
    terminal_result_t result;
    if (!allowed()) return result;
    result.topology_restored = restore();
    bool removed = true;
    for (const auto &name : outputs) {
      // Unknown helper completion fences further mutation, even for the killswitch.
      if (!allowed()) return result;
      if (!disconnect(name)) removed = false;
    }
    result.virtual_displays_removed = allowed() && verify(outputs) && allowed() && removed;
    return result;
  }

  /** Failed reconnects preserve the same paused-display policy as stream teardown. */
  inline std::optional<std::chrono::milliseconds> failed_preparation_restore_delay(
    bool app_paused,
    bool restore_on_disconnect,
    int paused_timeout_secs,
    std::chrono::milliseconds restore_delay
  ) {
    if (app_paused && !restore_on_disconnect) {
      if (paused_timeout_secs <= 0) return std::nullopt;
      return std::chrono::seconds(paused_timeout_secs);
    }
    return restore_delay;
  }

  /** Failed preparation must rearm the restore that it cancelled before mutation. */
  template <typename Cancel, typename Prepare, typename Restore>
  bool prepare_with_restore_on_failure(Cancel cancel, Prepare prepare, Restore restore) {
    cancel();
    try {
      if (prepare()) return true;
    } catch (...) {
      restore();
      throw;
    }
    restore();
    return false;
  }

  /**
   * Ownership is sampled under the lifecycle gate, before the display lock:
   * coordinator callbacks take the coordinator lock before the display lock.
   * New reservations cancel their old generation under the display lock, so a
   * callback already waiting for that lock cannot retire the new reservation.
   */
  template <typename ProtectedOwner, typename Restore, typename Complete = no_completion_t>
  result_e run_delayed_restore(
    std::mutex &lifecycle_gate,
    std::mutex &display_mutex,
    std::atomic<std::uint64_t> &generation,
    const std::uint64_t expected_generation,
    ProtectedOwner protected_owner,
    Restore restore,
    std::stop_token stop = {},
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max(),
    Complete complete = {},
    admission_e admission = admission_e::respect_owners
  ) {
    auto acquire = [&](std::unique_lock<std::mutex> &lock) {
      while (!lock.try_lock()) {
        if (stop.stop_requested() || generation.load(std::memory_order_acquire) != expected_generation ||
            std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return !stop.stop_requested() && std::chrono::steady_clock::now() < deadline;
    };
    std::unique_lock lifecycle_lock {lifecycle_gate, std::defer_lock};
    auto interrupted = [&] {
      return stop.stop_requested() || generation.load(std::memory_order_acquire) != expected_generation ?
               result_e::superseded : result_e::failed;
    };
    if (!acquire(lifecycle_lock)) return interrupted();
    if (generation.load(std::memory_order_acquire) != expected_generation) {
      return result_e::superseded;
    }
    if (admission == admission_e::respect_owners && protected_owner()) {
      return result_e::owned;
    }
    std::unique_lock display_lock {display_mutex, std::defer_lock};
    if (!acquire(display_lock)) return interrupted();
    if (generation.load(std::memory_order_acquire) != expected_generation) {
      return result_e::superseded;
    }
    auto claim = expected_generation;
    if (!generation.compare_exchange_strong(claim, expected_generation + 1, std::memory_order_acq_rel)) {
      return result_e::superseded;
    }
    if (!restore()) return result_e::failed;
    // Reconcile logical ownership before a new launch can enter, but after
    // releasing the display lock: coordinator callbacks acquire these locks
    // in the opposite order. A superseding cancellation cannot commit this
    // request's ownership cleanup.
    display_lock.unlock();
    if (!stop.stop_requested() && generation.load(std::memory_order_acquire) == expected_generation + 1) {
      complete(expected_generation + 1);
    }
    return result_e::restored;
  }

  /**
   * Retry confirmed failures without retaining either lock across backoff.
   * Each attempt rechecks ownership and claims only this request's generation;
   * a reconnect, replacement request or shutdown cancels all remaining work.
   * Uncertain helper completion must fence retries through retry_allowed.
   */
  template <typename ProtectedOwner, typename Restore, typename RetryAllowed, typename Complete = no_completion_t>
  result_e run_delayed_restore_with_retries(
    std::mutex &lifecycle_gate,
    std::mutex &display_mutex,
    std::atomic<std::uint64_t> &generation,
    std::uint64_t expected_generation,
    ProtectedOwner protected_owner,
    Restore restore,
    RetryAllowed retry_allowed,
    std::span<const std::chrono::milliseconds> retry_delays,
    std::stop_token stop = {},
    std::chrono::steady_clock::duration attempt_timeout = std::chrono::seconds(30),
    Complete complete = {}
  ) {
    for (std::size_t attempt = 0;; ++attempt) {
      bool claimed = false;
      const auto deadline = std::chrono::steady_clock::now() + attempt_timeout;
      const auto result = run_delayed_restore(
        lifecycle_gate, display_mutex, generation, expected_generation, protected_owner,
        [&] {
          claimed = true;
          return restore(expected_generation + 1, deadline);
        }, stop, deadline, complete);
      if (result != result_e::failed) return result;
      if (claimed) ++expected_generation;
      auto cancelled = [&] {
        return stop.stop_requested() || generation.load(std::memory_order_acquire) != expected_generation;
      };
      if (cancelled()) return result_e::superseded;
      if (attempt >= retry_delays.size() || !retry_allowed()) return result_e::failed;
      const auto due = std::chrono::steady_clock::now() + retry_delays[attempt];
      while (std::chrono::steady_clock::now() < due) {
        if (cancelled()) return result_e::superseded;
        if (!retry_allowed()) return result_e::failed;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (cancelled()) return result_e::superseded;
      if (!retry_allowed()) return result_e::failed;
    }
  }
}  // namespace platf::linux_private_display::cleanup_policy

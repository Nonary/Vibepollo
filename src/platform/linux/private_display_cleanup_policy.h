/**
 * @file src/platform/linux/private_display_cleanup_policy.h
 * @brief Serialize delayed display retirement with new stream ownership.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>

namespace platf::linux_private_display::cleanup_policy {
  enum class result_e {
    superseded,
    owned,
    restored,
    failed,
  };

  /**
   * Ownership is sampled under the lifecycle gate, before the display lock:
   * coordinator callbacks take the coordinator lock before the display lock.
   * New reservations cancel their old generation under the display lock, so a
   * callback already waiting for that lock cannot retire the new reservation.
   */
  template <typename ProtectedOwner, typename Restore>
  result_e run_delayed_restore(
    std::mutex &lifecycle_gate,
    std::mutex &display_mutex,
    std::atomic<std::uint64_t> &generation,
    const std::uint64_t expected_generation,
    ProtectedOwner protected_owner,
    Restore restore,
    std::stop_token stop = {},
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
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
    if (protected_owner()) {
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
    return restore() ? result_e::restored : result_e::failed;
  }
}  // namespace platf::linux_private_display::cleanup_policy

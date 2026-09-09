/**
 * @file src/platform/linux/private_display_cleanup_policy.h
 * @brief Serialize delayed display retirement with new stream ownership.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

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
    Restore restore
  ) {
    std::unique_lock lifecycle_lock {lifecycle_gate};
    if (generation.load(std::memory_order_acquire) != expected_generation) {
      return result_e::superseded;
    }
    if (protected_owner()) {
      return result_e::owned;
    }
    std::lock_guard display_lock {display_mutex};
    if (generation.load(std::memory_order_acquire) != expected_generation) {
      return result_e::superseded;
    }
    generation.fetch_add(1, std::memory_order_acq_rel);
    return restore() ? result_e::restored : result_e::failed;
  }
}  // namespace platf::linux_private_display::cleanup_policy

/**
 * @file shutdown_signal.h
 * @brief Process-wide cancellation visible before signal-driven cleanup.
 */
#pragma once

#include <atomic>

namespace lifetime {
  // Signal handlers may publish cancellation before they can wake main through
  // the mailbox. Keep this flag independent of cleanup ownership and locks.
  static_assert(std::atomic_bool::is_always_lock_free, "shutdown signal flag must be lock-free in a signal handler");
  inline std::atomic_bool shutdown_signal_requested {false};
}  // namespace lifetime

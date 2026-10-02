/**
 * @file src/deferred_stream_start_policy.h
 * @brief Non-blocking lifecycle admission for deferred stream-start work.
 */
#pragma once

#include <mutex>
#include <utility>

namespace stream::deferred_start {
  template<typename Mutex, typename Apply>
  bool try_apply_with_lifecycle_gate(Mutex &lifecycle_gate, Apply &&apply) {
    std::unique_lock<Mutex> lifecycle_lock {lifecycle_gate, std::try_to_lock};
    if (!lifecycle_lock.owns_lock()) {
      return false;
    }

    return std::forward<Apply>(apply)();
  }
}  // namespace stream::deferred_start

/**
 * @file src/deferred_action.h
 * @brief Small thread-safe one-shot deferred lifecycle state.
 */
#pragma once

#include <atomic>

namespace lifecycle {
  class deferred_action_t {
  public:
    void defer() noexcept {
      pending_.store(true, std::memory_order_release);
    }

    bool consume() noexcept {
      return pending_.exchange(false, std::memory_order_acq_rel);
    }

    void clear() noexcept {
      pending_.store(false, std::memory_order_release);
    }

  private:
    std::atomic_bool pending_ {false};
  };
}  // namespace lifecycle

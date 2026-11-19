/** @file Linux capture target discovery and its cancellation deadline. */
#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>

namespace platf {
  enum class display_switch_source_e { named, indexed, kms };

  struct capture_deadline_t {
    using clock = std::chrono::steady_clock;
    clock::time_point until = clock::time_point::max();
    std::function<bool()> allowed;

    bool active() const {
      return clock::now() < until && (!allowed || allowed());
    }

    std::unique_lock<std::timed_mutex> lock(std::timed_mutex &mutex) const {
      std::unique_lock result {mutex, std::defer_lock};
      if (!allowed) {
        if (until == clock::time_point::max()) {
          result.lock();
        } else if (active()) {
          (void) result.try_lock_until(until);
        }
      } else {
        while (active()) {
          if (result.try_lock_until(std::min(until, clock::now() + std::chrono::milliseconds(50)))) break;
        }
      }
      return result;
    }
  };
}  // namespace platf

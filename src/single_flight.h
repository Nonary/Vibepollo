/**
 * @file src/single_flight.h
 * @brief Admit one queued or running operation without blocking its callers.
 */
#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace single_flight {
  class admission_t {
    struct lease_t {
      std::shared_ptr<std::atomic_bool> busy;

      ~lease_t() {
        busy->store(false, std::memory_order_release);
      }
    };

    std::shared_ptr<std::atomic_bool> busy_ = std::make_shared<std::atomic_bool>(false);

  public:
    template<class Submit, class Task>
    bool try_submit(Submit &&submit, Task &&task) {
      if (busy_->exchange(true, std::memory_order_acq_rel)) {
        return false;
      }
      std::shared_ptr<lease_t> lease;
      try {
        lease = std::make_shared<lease_t>();
      } catch (...) {
        busy_->store(false, std::memory_order_release);
        throw;
      }
      lease->busy = busy_;
      std::forward<Submit>(submit)([lease = std::move(lease), task = std::forward<Task>(task)]() mutable {
        // Release when execution ends, even if the executor retains the task
        // object or the operation throws. A discarded queued task releases too.
        auto running = std::move(lease);
        task();
      });
      return true;
    }
  };
}  // namespace single_flight

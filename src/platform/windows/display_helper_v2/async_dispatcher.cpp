#include "src/platform/windows/display_helper_v2/async_dispatcher.h"

namespace display_helper::v2 {
  AsyncDispatcher::AsyncDispatcher(
    ApplyOperation &apply_operation,
    VerificationOperation &verification_operation,
    RecoveryOperation &recovery_operation,
    RecoveryValidationOperation &recovery_validation_operation,
    IVirtualDisplayDriver &virtual_display,
    IClock &clock)
    : apply_operation_(apply_operation),
      verification_operation_(verification_operation),
      recovery_operation_(recovery_operation),
      recovery_validation_operation_(recovery_validation_operation),
      virtual_display_(virtual_display),
      clock_(clock),
      worker_(&AsyncDispatcher::worker_loop, this) {}

  AsyncDispatcher::~AsyncDispatcher() {
    if (worker_.joinable()) {
      worker_.request_stop();
      cv_.notify_all();
      worker_.join();
    }
  }

  void AsyncDispatcher::dispatch_apply(
    const ApplyRequest &request,
    const CancellationToken &token,
    std::chrono::milliseconds delay,
    bool reset_virtual_display,
    std::function<void(const ApplyOutcome &)> completion) {
    enqueue_task([
      this,
      request,
      token,
      delay,
      reset_virtual_display,
      completion = std::move(completion)
    ]() mutable {
      if (delay > std::chrono::milliseconds::zero()) {
        clock_.sleep_for(delay);
      }

      if (reset_virtual_display) {
        if (!virtual_display_.disable()) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Fatal;
          completion(outcome);
          return;
        }
        clock_.sleep_for(std::chrono::milliseconds(500));
        if (!virtual_display_.enable()) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Fatal;
          completion(outcome);
          return;
        }
        clock_.sleep_for(std::chrono::milliseconds(1000));
      }

      completion(apply_operation_.run(request, token));
    });
  }

  void AsyncDispatcher::dispatch_verification(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const CancellationToken &token,
    std::function<void(bool)> completion) {
    enqueue_task([
      this,
      request,
      expected_topology,
      token,
      completion = std::move(completion)
    ]() mutable {
      completion(verification_operation_.run(request, expected_topology, token));
    });
  }

  void AsyncDispatcher::dispatch_recovery(
    const CancellationToken &token,
    std::function<void(const RecoveryOutcome &)> completion) {
    enqueue_task([
      this,
      token,
      completion = std::move(completion)
    ]() mutable {
      completion(recovery_operation_.run(token));
    });
  }

  void AsyncDispatcher::dispatch_recovery_validation(
    const Snapshot &snapshot,
    const CancellationToken &token,
    std::function<void(bool)> completion) {
    enqueue_task([
      this,
      snapshot,
      token,
      completion = std::move(completion)
    ]() mutable {
      completion(recovery_validation_operation_.run(snapshot, token));
    });
  }

  void AsyncDispatcher::enqueue_task(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  void AsyncDispatcher::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return st.stop_requested() || !tasks_.empty(); });
        if (st.stop_requested()) {
          break;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }

      if (task) {
        task();
      }
    }
  }
}  // namespace display_helper::v2

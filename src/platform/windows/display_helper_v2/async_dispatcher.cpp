#include "src/platform/windows/display_helper_v2/async_dispatcher.h"

#include "src/logging.h"

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
      worker_(&AsyncDispatcher::worker_loop, this),
      timer_worker_(&AsyncDispatcher::timer_loop, this) {}

  AsyncDispatcher::~AsyncDispatcher() {
    if (timer_worker_.joinable()) {
      timer_worker_.request_stop();
      timer_cv_.notify_all();
      timer_worker_.join();
    }
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
      auto remaining_delay = delay;
      constexpr auto kCancellationSlice = std::chrono::milliseconds(100);
      while (remaining_delay > std::chrono::milliseconds::zero()) {
        if (token.is_cancelled()) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Fatal;
          completion(outcome);
          return;
        }
        const auto slice = remaining_delay > kCancellationSlice ? kCancellationSlice : remaining_delay;
        clock_.sleep_for(slice);
        remaining_delay -= slice;
      }

      ApplyOutcome virtual_reset_outcome;
      virtual_reset_outcome.virtual_display_requested = request.virtual_layout.has_value();
      if (reset_virtual_display) {
        if (token.is_cancelled()) {
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        // Closing/reopening the virtual-display handle changes Windows'
        // display stack before ApplyOperation can reach its topology boundary.
        // Use the same durable guard first, then carry that fact through every
        // early return so the state machine cannot discard recovery too soon.
        virtual_reset_outcome.durable_recovery_armed =
          apply_operation_.arm_durable_recovery_boundary();
        if (token.is_cancelled()) {
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        virtual_reset_outcome.display_may_have_changed = true;
        if (!virtual_display_.disable()) {
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        bool virtual_display_disabled = true;
        auto restore_disabled_device = [&]() {
          if (virtual_display_disabled && !virtual_display_.enable()) {
            BOOST_LOG(warning) << "Display helper v2: failed to re-enable virtual display after cancellation.";
          }
          virtual_display_disabled = false;
        };
        auto sleep_with_cancel = [&](std::chrono::milliseconds duration) {
          constexpr auto kCancellationSlice = std::chrono::milliseconds(100);
          auto remaining = duration;
          while (remaining > std::chrono::milliseconds::zero()) {
            if (token.is_cancelled()) {
              return false;
            }
            const auto slice = remaining > kCancellationSlice ? kCancellationSlice : remaining;
            clock_.sleep_for(slice);
            remaining -= slice;
          }
          return !token.is_cancelled();
        };
        if (!sleep_with_cancel(std::chrono::milliseconds(500))) {
          restore_disabled_device();
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        if (token.is_cancelled()) {
          restore_disabled_device();
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        if (!virtual_display_.enable()) {
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
        virtual_display_disabled = false;
        if (!sleep_with_cancel(std::chrono::milliseconds(1000))) {
          virtual_reset_outcome.status = ApplyStatus::Fatal;
          completion(virtual_reset_outcome);
          return;
        }
      }

      auto outcome = apply_operation_.run(
        request,
        token,
        virtual_reset_outcome.durable_recovery_armed);
      outcome.virtual_display_requested = outcome.virtual_display_requested ||
                                         virtual_reset_outcome.virtual_display_requested;
      outcome.display_may_have_changed = outcome.display_may_have_changed ||
                                           virtual_reset_outcome.display_may_have_changed;
      outcome.durable_recovery_armed = outcome.durable_recovery_armed ||
                                        virtual_reset_outcome.durable_recovery_armed;
      completion(outcome);
    });
  }

  void AsyncDispatcher::dispatch_verification(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const std::optional<ResolvedConfigurationTarget> &resolved_target,
    const CancellationToken &token,
    std::function<void(bool)> completion) {
    enqueue_task([
      this,
      request,
      expected_topology,
      resolved_target,
      token,
      completion = std::move(completion)
    ]() mutable {
      completion(verification_operation_.run(request, expected_topology, resolved_target, token));
    });
  }

  void AsyncDispatcher::dispatch_verification_after(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const std::optional<ResolvedConfigurationTarget> &resolved_target,
    const CancellationToken &token,
    std::chrono::milliseconds delay,
    std::function<void(bool)> completion) {
    enqueue_delayed_task([
      this,
      request,
      expected_topology,
      resolved_target,
      token,
      delay,
      completion = std::move(completion)
    ](std::stop_token stop_token) mutable {
      auto remaining_delay = delay;
      constexpr auto kCancellationSlice = std::chrono::milliseconds(100);
      while (remaining_delay > std::chrono::milliseconds::zero()) {
        if (stop_token.stop_requested() || token.is_cancelled()) {
          return;
        }
        const auto slice = remaining_delay > kCancellationSlice ? kCancellationSlice : remaining_delay;
        clock_.sleep_for(slice);
        remaining_delay -= slice;
      }
      if (stop_token.stop_requested() || token.is_cancelled()) {
        return;
      }
      enqueue_task([
        this,
        request,
        expected_topology,
        resolved_target,
        token,
        completion = std::move(completion)
      ]() mutable {
        completion(verification_operation_.run(request, expected_topology, resolved_target, token));
      });
    });
  }

  void AsyncDispatcher::dispatch_reset_staged_apply_state(
    std::function<void(bool)> completion) {
    enqueue_task([
      this,
      completion = std::move(completion)
    ]() mutable {
      // RESET is deliberately non-cancellable once ordered behind prior work.
      // A subsequent APPLY is queued after this task and observes the reset.
      completion(apply_operation_.reset_staged_apply_state());
    });
  }

  void AsyncDispatcher::dispatch_recovery(
    const CancellationToken &token,
    std::chrono::milliseconds delay,
    std::function<void(const RecoveryOutcome &)> completion) {
    enqueue_task([
      this,
      token,
      delay,
      completion = std::move(completion)
    ]() mutable {
      // Sleep in slices so a DISARM/APPLY during the revert grace window can
      // cancel the pending restore before it touches the display stack.
      auto remaining = delay;
      constexpr auto kSlice = std::chrono::milliseconds(50);
      while (remaining > std::chrono::milliseconds::zero()) {
        if (token.is_cancelled()) {
          completion(RecoveryOutcome {});
          return;
        }
        const auto slice = remaining > kSlice ? kSlice : remaining;
        clock_.sleep_for(slice);
        remaining -= slice;
      }

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

  void AsyncDispatcher::dispatch_refresh_rate(
    std::string device_id,
    unsigned int numerator,
    unsigned int denominator,
    const CancellationToken &token,
    std::function<void(bool)> completion
  ) {
    enqueue_task([
      this,
      device_id = std::move(device_id),
      numerator,
      denominator,
      token,
      completion = std::move(completion)
    ]() mutable {
      if (token.is_cancelled()) {
        completion(false);
        return;
      }
      completion(apply_operation_.set_refresh_rate(device_id, numerator, denominator));
    });
  }

  void AsyncDispatcher::enqueue_task(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  void AsyncDispatcher::enqueue_delayed_task(std::function<void(std::stop_token)> task) {
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      timer_tasks_.push_back(std::move(task));
    }
    timer_cv_.notify_one();
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

  void AsyncDispatcher::timer_loop(std::stop_token st) {
    while (!st.stop_requested()) {
      std::function<void(std::stop_token)> task;
      {
        std::unique_lock<std::mutex> lock(timer_mutex_);
        timer_cv_.wait(lock, [&]() { return st.stop_requested() || !timer_tasks_.empty(); });
        if (st.stop_requested()) {
          break;
        }
        task = std::move(timer_tasks_.front());
        timer_tasks_.pop_front();
      }
      if (task) {
        task(st);
      }
    }
  }
}  // namespace display_helper::v2

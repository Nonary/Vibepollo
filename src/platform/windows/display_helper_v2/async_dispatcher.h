#pragma once

#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace display_helper::v2 {
  class IAsyncDispatcher {
  public:
    virtual ~IAsyncDispatcher() = default;

    virtual void dispatch_apply(
      const ApplyRequest &request,
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      bool reset_virtual_display,
      std::function<void(const ApplyOutcome &)> completion) = 0;

    virtual void dispatch_verification(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      const CancellationToken &token,
      std::function<void(bool)> completion) = 0;

    /// Queue a delayed health verification without blocking the state-machine
    /// thread. Simple test dispatchers can use the default immediate behavior.
    virtual void dispatch_verification_after(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      std::function<void(bool)> completion) {
      (void) delay;
      dispatch_verification(request, expected_topology, resolved_target, token, std::move(completion));
    }

    /// Reset state retained by the settings backend only after preceding work
    /// in the dispatcher has reached a cancellation point. RESET is a barrier:
    /// a following APPLY must not be allowed to cancel it.
    virtual void dispatch_reset_staged_apply_state(
      std::function<void(bool)> completion) {
      completion(false);
    }

    virtual void dispatch_refresh_rate(
      std::string,
      unsigned int,
      unsigned int,
      const CancellationToken &,
      std::function<void(bool)> completion) {
      completion(false);
    }

    virtual void dispatch_recovery(
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      std::function<void(const RecoveryOutcome &)> completion) = 0;

    virtual void dispatch_recovery_validation(
      const Snapshot &snapshot,
      const CancellationToken &token,
      std::function<void(bool)> completion) = 0;
  };

  class AsyncDispatcher final : public IAsyncDispatcher {
  public:
    AsyncDispatcher(
      ApplyOperation &apply_operation,
      VerificationOperation &verification_operation,
      RecoveryOperation &recovery_operation,
      RecoveryValidationOperation &recovery_validation_operation,
      IVirtualDisplayDriver &virtual_display,
      IClock &clock);

    ~AsyncDispatcher();

    void dispatch_apply(
      const ApplyRequest &request,
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      bool reset_virtual_display,
      std::function<void(const ApplyOutcome &)> completion) override;

    void dispatch_verification(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      const CancellationToken &token,
      std::function<void(bool)> completion) override;

    void dispatch_verification_after(
      const ApplyRequest &request,
      const std::optional<ActiveTopology> &expected_topology,
      const std::optional<ResolvedConfigurationTarget> &resolved_target,
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      std::function<void(bool)> completion) override;

    void dispatch_reset_staged_apply_state(
      std::function<void(bool)> completion) override;

    void dispatch_refresh_rate(
      std::string device_id,
      unsigned int numerator,
      unsigned int denominator,
      const CancellationToken &token,
      std::function<void(bool)> completion) override;

    void dispatch_recovery(
      const CancellationToken &token,
      std::chrono::milliseconds delay,
      std::function<void(const RecoveryOutcome &)> completion) override;

    void dispatch_recovery_validation(
      const Snapshot &snapshot,
      const CancellationToken &token,
      std::function<void(bool)> completion) override;

  private:
    void enqueue_task(std::function<void()> task);
    void enqueue_delayed_task(std::function<void(std::stop_token)> task);
    void worker_loop(std::stop_token st);
    void timer_loop(std::stop_token st);

    ApplyOperation &apply_operation_;
    VerificationOperation &verification_operation_;
    RecoveryOperation &recovery_operation_;
    RecoveryValidationOperation &recovery_validation_operation_;
    IVirtualDisplayDriver &virtual_display_;
    IClock &clock_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::jthread worker_;

    // Delays are scheduled off the serialized mutation worker. A 5.5-second
    // HDR settling check must not block an immediate refresh or RESET command.
    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::deque<std::function<void(std::stop_token)>> timer_tasks_;
    std::jthread timer_worker_;
  };
}  // namespace display_helper::v2

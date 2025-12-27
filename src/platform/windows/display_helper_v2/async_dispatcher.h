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
      const CancellationToken &token,
      std::function<void(bool)> completion) = 0;

    virtual void dispatch_recovery(
      const CancellationToken &token,
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
      const CancellationToken &token,
      std::function<void(bool)> completion) override;

    void dispatch_recovery(
      const CancellationToken &token,
      std::function<void(const RecoveryOutcome &)> completion) override;

    void dispatch_recovery_validation(
      const Snapshot &snapshot,
      const CancellationToken &token,
      std::function<void(bool)> completion) override;

  private:
    void enqueue_task(std::function<void()> task);
    void worker_loop(std::stop_token st);

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
  };
}  // namespace display_helper::v2

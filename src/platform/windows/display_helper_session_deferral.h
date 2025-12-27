#pragma once

#include "src/display_helper_builder.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace display_helper_integration {
  class SessionDeferralManager {
  public:
    struct PendingSessionSnapshot {
      int width = 0;
      int height = 0;
      int fps = 0;
      bool enable_hdr = false;
      bool enable_sops = false;
      bool virtual_display = false;
      std::string virtual_display_device_id;
      std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
      std::optional<int> framegen_refresh_rate;
      bool gen1_framegen_fix = false;
      bool gen2_framegen_fix = false;
    };

    struct PendingApplyState {
      DisplayApplyRequest request;
      PendingSessionSnapshot session_snapshot;
      std::uint32_t session_id {0};
      bool has_session {false};
      int attempts {0};
      std::optional<std::chrono::steady_clock::time_point> ready_since;
      std::chrono::steady_clock::time_point next_attempt {};
    };

    enum class TakeStatus {
      NoPending,
      SessionNotReady,
      DelayStarted,
      DelayPending,
      Ready,
      DroppedMaxAttempts,
    };

    struct TakeResult {
      TakeStatus status {TakeStatus::NoPending};
      std::optional<PendingApplyState> pending;
    };

    struct RescheduleResult {
      bool requeued = false;
      bool dropped_for_newer = false;
      bool dropped_max_attempts = false;
      int attempts = 0;
      std::chrono::milliseconds delay {0};
    };

    using NowFn = std::function<std::chrono::steady_clock::time_point()>;

    explicit SessionDeferralManager(NowFn now_fn);

    void set_pending(const DisplayApplyRequest &request);
    TakeResult take_ready(bool session_ready);
    RescheduleResult reschedule(PendingApplyState pending);
    void clear();
    bool has_pending() const;

    static std::chrono::milliseconds retry_delay(int attempts);
    static std::chrono::milliseconds initial_delay();
    static int max_attempts();

  private:
    PendingApplyState make_state(const DisplayApplyRequest &request) const;

    NowFn now_fn_;
    mutable std::mutex mutex_;
    std::optional<PendingApplyState> pending_;
  };
}  // namespace display_helper_integration

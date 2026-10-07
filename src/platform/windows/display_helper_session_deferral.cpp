#include "src/platform/windows/display_helper_session_deferral.h"

#include <algorithm>

namespace display_helper_integration {
  namespace {
    constexpr std::chrono::milliseconds kDeferredApplyInitialDelay {2000};
    constexpr std::chrono::milliseconds kDeferredApplyRetryBase {500};
    constexpr std::chrono::milliseconds kDeferredApplyRetryMax {10000};
    constexpr int kMaxDeferredApplyAttempts = 6;
  }  // namespace

  SessionDeferralManager::SessionDeferralManager(NowFn now_fn):
      now_fn_(std::move(now_fn)) {}

  void SessionDeferralManager::set_pending(
    const DisplayApplyRequest &request,
    PendingSessionSnapshot session_snapshot,
    const std::uint32_t session_id,
    const bool has_session
  ) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = make_state(request, std::move(session_snapshot), session_id, has_session);
  }

  SessionDeferralManager::TakeResult SessionDeferralManager::take_ready(bool session_ready) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) {
      return {TakeStatus::NoPending, std::nullopt};
    }

    if (!session_ready) {
      return {TakeStatus::SessionNotReady, std::nullopt};
    }

    auto &state = *pending_;
    const auto now = now_fn_();
    if (!state.ready_since) {
      state.ready_since = now;
      state.next_attempt = now + kDeferredApplyInitialDelay;
      return {TakeStatus::DelayStarted, std::nullopt};
    }

    if (now < state.next_attempt) {
      return {TakeStatus::DelayPending, std::nullopt};
    }

    if (state.attempts >= kMaxDeferredApplyAttempts) {
      pending_.reset();
      return {TakeStatus::DroppedMaxAttempts, std::nullopt};
    }

    PendingApplyState ready = std::move(state);
    pending_.reset();
    return {TakeStatus::Ready, std::move(ready)};
  }

  SessionDeferralManager::RescheduleResult SessionDeferralManager::reschedule(PendingApplyState pending) {
    RescheduleResult result;
    std::lock_guard<std::mutex> lock(mutex_);

    if (pending.attempts >= kMaxDeferredApplyAttempts) {
      result.dropped_max_attempts = true;
      return result;
    }

    pending.attempts += 1;
    result.attempts = pending.attempts;
    result.delay = retry_delay(pending.attempts);
    pending.next_attempt = now_fn_() + result.delay;
    if (!pending.ready_since) {
      pending.ready_since = now_fn_();
    }

    if (pending_) {
      result.dropped_for_newer = true;
      return result;
    }

    pending_ = std::move(pending);
    result.requeued = true;
    return result;
  }

  void SessionDeferralManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
  }

  bool SessionDeferralManager::has_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.has_value();
  }

  std::chrono::milliseconds SessionDeferralManager::retry_delay(int attempts) {
    if (attempts <= 0) {
      return kDeferredApplyRetryBase;
    }
    const int shift = std::min(attempts - 1, 5);
    auto delay = kDeferredApplyRetryBase * (1 << shift);
    if (delay > kDeferredApplyRetryMax) {
      delay = kDeferredApplyRetryMax;
    }
    return delay;
  }

  std::chrono::milliseconds SessionDeferralManager::initial_delay() {
    return kDeferredApplyInitialDelay;
  }

  int SessionDeferralManager::max_attempts() {
    return kMaxDeferredApplyAttempts;
  }

  SessionDeferralManager::PendingApplyState SessionDeferralManager::make_state(
    const DisplayApplyRequest &request,
    PendingSessionSnapshot session_snapshot,
    const std::uint32_t session_id,
    const bool has_session
  ) const {
    PendingApplyState state;
    state.request = request;
    state.has_session = has_session;
    state.request.session = nullptr;
    state.session_id = session_id;
    state.session_snapshot = std::move(session_snapshot);

    return state;
  }
}  // namespace display_helper_integration

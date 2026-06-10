#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

/**
 * @file golden_health.h
 * @brief Golden-restore health tracking ported verbatim from the legacy display helper.
 *
 * Tracks unresolved golden restore requests in display_golden_restore_status.json and
 * only warns that the saved snapshot may be out of date after repeated failures persist
 * across a long window (3 attempts over 72h), so transient failures stay quiet.
 */
namespace display_helper::v2 {
  class GoldenHealth {
  public:
    static constexpr size_t kGoldenOutOfDateFailureThreshold = 3;
    static constexpr auto kGoldenOutOfDateFailureWindow = std::chrono::hours(72);

    using NowMsProvider = std::function<long long()>;

    explicit GoldenHealth(std::filesystem::path status_path, NowMsProvider now_ms = {});

    /// Remove the status marker (restore confirmed / snapshot exported) and reset tracking.
    void clear_status(const char *reason);

    /// Forget any issue noted for the current restore request.
    void reset_request_tracking();

    /// Note that the current restore request hit a golden-restore issue.
    void note_issue(const char *reason);

    /// If the current request had an unresolved issue, persist it to the status marker
    /// and warn once failures persist past the threshold/window.
    void register_unresolved(const char *context);

  private:
    std::filesystem::path status_path_;
    NowMsProvider now_ms_;
    std::mutex mutex_;
    bool had_issue_this_request_ {false};
    std::string last_issue_;
  };
}  // namespace display_helper::v2

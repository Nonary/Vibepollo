#include "src/platform/windows/display_helper_v2/golden_health.h"

#ifdef _WIN32

  #include <algorithm>
  #include <fstream>

  #include <nlohmann/json.hpp>

  #include "src/logging.h"
  #include "src/platform/windows/display_helper_v2/snapshot_codec.h"

namespace display_helper::v2 {
  GoldenHealth::GoldenHealth(std::filesystem::path status_path, NowMsProvider now_ms)
    : status_path_(std::move(status_path)),
      now_ms_(std::move(now_ms)) {
    if (!now_ms_) {
      now_ms_ = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()
        )
          .count();
      };
    }
  }

  void GoldenHealth::clear_status(const char *reason) {
    reset_request_tracking();
    if (status_path_.empty()) {
      return;
    }

    std::error_code ec;
    const bool removed = std::filesystem::remove(status_path_, ec);
    if (removed && !ec) {
      BOOST_LOG(info) << "Golden restore health reset"
                      << (reason ? std::string(" (") + reason + ")" : "") << ".";
    }
  }

  void GoldenHealth::reset_request_tracking() {
    std::lock_guard<std::mutex> lock(mutex_);
    had_issue_this_request_ = false;
    last_issue_.clear();
  }

  void GoldenHealth::note_issue(const char *reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    had_issue_this_request_ = true;
    last_issue_ = reason && *reason ? reason : "restore_failed";
  }

  void GoldenHealth::register_unresolved(const char *context) {
    std::string reason;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!had_issue_this_request_) {
        return;
      }
      reason = last_issue_.empty() ? "restore_failed" : last_issue_;
      had_issue_this_request_ = false;
      last_issue_.clear();
    }

    const auto now_ms = now_ms_();

    long long first_failure_ms = now_ms;
    size_t failures = 0;
    if (!status_path_.empty()) {
      try {
        std::ifstream file(status_path_, std::ios::binary);
        if (file.is_open()) {
          auto previous = nlohmann::json::parse(file, nullptr, false);
          if (!previous.is_discarded() && previous.is_object()) {
            first_failure_ms = previous.value("first_failure_unix_ms", first_failure_ms);
            failures = static_cast<size_t>(previous.value("unresolved_restore_attempts", 0ull));
          }
        }
      } catch (...) {
      }
    }

    failures += 1;
    const auto failure_window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     kGoldenOutOfDateFailureWindow
    )
                                     .count();
    const auto unresolved_ms = std::max<long long>(0, now_ms - first_failure_ms);
    const bool enough_attempts = failures >= kGoldenOutOfDateFailureThreshold;
    const bool old_enough = unresolved_ms >= failure_window_ms;
    const bool should_warn = enough_attempts && old_enough;

    nlohmann::json status;
    status["snapshot_out_of_date"] = should_warn;
    status["reason"] = should_warn ? "restore_failed_for_days" : reason;
    status["last_failure_reason"] = reason;
    status["unresolved_restore_attempts"] = failures;
    status["failure_threshold"] = kGoldenOutOfDateFailureThreshold;
    status["failure_window_hours"] = std::chrono::duration_cast<std::chrono::hours>(
                                       kGoldenOutOfDateFailureWindow
    )
                                       .count();
    status["first_failure_unix_ms"] = first_failure_ms;
    status["latest_failure_unix_ms"] = now_ms;
    status["updated_at_unix_ms"] = now_ms;

    if (!codec::write_text_atomically(status.dump(2) + "\n", status_path_)) {
      BOOST_LOG(warning) << "Golden restore remained unresolved"
                         << (context ? std::string(" (") + context + ")" : "")
                         << ", but failed to write restore health marker.";
      return;
    }

    if (should_warn) {
      BOOST_LOG(warning) << "Golden restore has remained unresolved for "
                         << std::chrono::duration_cast<std::chrono::hours>(
                              std::chrono::milliseconds(unresolved_ms)
                            )
                              .count()
                         << "h across " << failures
                         << " restore request(s); marking saved display snapshot as possibly out of date.";
    } else {
      BOOST_LOG(info) << "Golden restore remained unresolved"
                      << (context ? std::string(" (") + context + ")" : "")
                      << " (" << reason << "); observing for "
                      << kGoldenOutOfDateFailureThreshold << " attempts over "
                      << std::chrono::duration_cast<std::chrono::hours>(kGoldenOutOfDateFailureWindow).count()
                      << "h before warning.";
    }
  }
}  // namespace display_helper::v2

#endif  // _WIN32

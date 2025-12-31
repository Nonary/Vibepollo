#include "src/platform/windows/display_helper_watchdog.h"

#include <utility>

#include "src/logging.h"

using namespace std::chrono_literals;

namespace display_helper_integration {
  DisplayHelperWatchdog::DisplayHelperWatchdog(Hooks hooks)
    : hooks_(std::move(hooks)) {}

  std::chrono::milliseconds DisplayHelperWatchdog::tick() {
    const auto interval = (hooks_.session_count && hooks_.running_processes &&
                           hooks_.session_count() == 0 && hooks_.running_processes() > 0)
                            ? suspended_interval()
                            : active_interval();

    if (hooks_.feature_enabled && !hooks_.feature_enabled()) {
      if (helper_ready_) {
        BOOST_LOG(info) << "Display helper watchdog: feature disabled, releasing helper connection.";
        if (hooks_.reset_connection) {
          hooks_.reset_connection();
        }
      }
      helper_ready_ = false;
      return interval;
    }

    if (!helper_ready_) {
      if (hooks_.ensure_helper_started && hooks_.ensure_helper_started()) {
        helper_ready_ = true;
        if (hooks_.send_ping) {
          (void) hooks_.send_ping();
        }
        return interval;
      }
      helper_ready_ = false;
      return interval;
    }

    if (hooks_.send_ping && !hooks_.send_ping()) {
      BOOST_LOG(warning) << "Display helper watchdog: ping failed, helper may have crashed or become unresponsive.";
      if (hooks_.reset_connection) {
        hooks_.reset_connection();
      }
      if (hooks_.ensure_helper_started && hooks_.ensure_helper_started()) {
        helper_ready_ = hooks_.send_ping ? hooks_.send_ping() : true;
        if (helper_ready_) {
          BOOST_LOG(info) << "Display helper watchdog: successfully restarted helper after ping failure.";
        } else {
          BOOST_LOG(warning) << "Display helper watchdog: helper restarted but ping still failing.";
        }
      } else {
        BOOST_LOG(error) << "Display helper watchdog: failed to restart helper after ping failure.";
        helper_ready_ = false;
      }
    }

    return interval;
  }

  void DisplayHelperWatchdog::reset() {
    helper_ready_ = false;
  }

  bool DisplayHelperWatchdog::helper_ready() const {
    return helper_ready_;
  }

  std::chrono::milliseconds DisplayHelperWatchdog::active_interval() {
    return 10s;
  }

  std::chrono::milliseconds DisplayHelperWatchdog::suspended_interval() {
    return 20s;
  }
}  // namespace display_helper_integration

#include "src/platform/windows/display_helper_watchdog.h"

#include <utility>

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
      if (helper_ready_ && hooks_.reset_connection) {
        hooks_.reset_connection();
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
      if (hooks_.reset_connection) {
        hooks_.reset_connection();
      }
      if (hooks_.ensure_helper_started && hooks_.ensure_helper_started()) {
        helper_ready_ = hooks_.send_ping ? hooks_.send_ping() : true;
      } else {
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

#include "display_helper_shell_refresh_policy.h"

namespace display_helper::v2::shell_refresh_policy {
  bool allows_unbounded_system_parameter_broadcast() {
    return false;
  }

  bool defers_window_broadcasts_from_capture_gate() {
    return true;
  }
}  // namespace display_helper::v2::shell_refresh_policy

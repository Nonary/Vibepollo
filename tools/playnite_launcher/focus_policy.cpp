#include "tools/playnite_launcher/focus_policy.h"

#include <algorithm>

namespace playnite_launcher::focus::policy {

  clock::time_point focus_deadline(
    clock::time_point armed_at,
    std::optional<clock::time_point> last_new_window_at,
    std::chrono::seconds focus_timeout
  ) {
    const auto hard_limit = armed_at + std::max(k_window_wait_limit, focus_timeout);
    if (!last_new_window_at) {
      return hard_limit;
    }
    const auto deadline = std::max(armed_at, *last_new_window_at) + focus_timeout;
    return std::min(deadline, hard_limit);
  }

}  // namespace playnite_launcher::focus::policy

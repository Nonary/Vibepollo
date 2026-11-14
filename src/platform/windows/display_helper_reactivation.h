#pragma once

#include <functional>
#include <string>

namespace display_helper_integration {
  // Serialize against APPLY/REVERT before validating driver ownership. The
  // callback must try-lock the display operation gate to avoid lock inversion.
  bool reactivate_owned_virtual_display(
    const std::string &device_id,
    const std::function<bool()> &validate_owner,
    std::function<bool()> cancelled
  );

  // Run a host-side layout repair for an owned live target, or decline without
  // waiting when an APPLY, a REVERT or a restore owns the desktop. The repair
  // runs with the display mutation gate held. Returns whether it ran.
  bool run_owned_layout_repair(
    const std::function<bool()> &validate_owner,
    const std::function<void()> &repair,
    const std::function<bool()> &cancelled
  );
}  // namespace display_helper_integration

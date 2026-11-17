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
}  // namespace display_helper_integration

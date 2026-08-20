/**
 * @file src/app_display_policy.h
 * @brief Pure precedence rules for application display selection.
 */
#pragma once

namespace proc::display_policy {
  enum class app_override_e {
    inherit,
    physical,
    virtual_display,
  };

  constexpr bool resolve_virtual_display_request(
    bool inherited_virtual_request,
    app_override_e app_override
  ) {
    switch (app_override) {
      case app_override_e::physical:
        return false;
      case app_override_e::virtual_display:
        return true;
      case app_override_e::inherit:
        return inherited_virtual_request;
    }
    return inherited_virtual_request;
  }
}  // namespace proc::display_policy

#pragma once

namespace color_profile_policy {
  // A failed supported API must never be replaced by a registry-only association.
  template<class Native, class Legacy>
  bool associate(Native native, Legacy legacy) {
    const auto result = native();
    return result.api_available ? result.success : legacy();
  }
}

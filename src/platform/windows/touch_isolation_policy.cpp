/**
 * @file src/platform/windows/touch_isolation_policy.cpp
 */

#include "touch_isolation_policy.h"

#include <algorithm>

namespace platf::touch_isolation {
  bool is_empty(const rect_t &rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
  }

  bool is_promoted_from_pointer(const std::uint64_t extra_info) {
    return (extra_info & POINTER_SIGNATURE_MASK) == POINTER_SIGNATURE;
  }

  bool contains(const rect_t &rect, const int x, const int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
  }

  bool should_swallow(const std::uint64_t extra_info, const int x, const int y, const rect_t &rect) {
    if (is_empty(rect)) {
      return false;
    }

    // Input the host injects itself carries no extra info, so the client's own mouse is
    // never caught here. Only promotion from a pen or touch contact is.
    if (!is_promoted_from_pointer(extra_info)) {
      return false;
    }

    return contains(rect, x, y);
  }

  std::optional<rect_t> match_virtual_display_rect(
    const rect_t &streamed,
    const std::span<const rect_t> virtual_display_rects,
    const std::span<const rect_t> all_monitor_rects
  ) {
    if (is_empty(streamed)) {
      return std::nullopt;
    }

    const auto matched = std::find(virtual_display_rects.begin(), virtual_display_rects.end(), streamed);
    if (matched == virtual_display_rects.end()) {
      return std::nullopt;
    }

    const auto occurrences = std::count(all_monitor_rects.begin(), all_monitor_rects.end(), streamed);
    if (occurrences > 1) {
      return std::nullopt;
    }

    return *matched;
  }
}  // namespace platf::touch_isolation

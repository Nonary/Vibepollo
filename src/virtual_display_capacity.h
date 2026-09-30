#pragma once

#include <cstddef>

namespace virtual_display_capacity {
  inline constexpr std::size_t default_clients = 4;
  // The published Windows driver exposes eight temporary per-client monitors.
  inline constexpr std::size_t max_clients = 8;
}  // namespace virtual_display_capacity

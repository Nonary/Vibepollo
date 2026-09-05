/** @file Pure policy for the patched Gamescope HDR10 capture contract. */
#pragma once

#include <cstdint>

namespace platf::gamescope_hdr {
  inline constexpr std::uint32_t profile_v1 = 1;

  constexpr bool supported(std::uint32_t profile, std::uint32_t profile_node, std::uint32_t capture_node) {
    return profile == profile_v1 && capture_node != UINT32_MAX && profile_node == capture_node;
  }

  constexpr bool negotiated(bool requested, bool capable, bool rgb10, bool bt2020, bool pq, bool full_range) {
    return requested && capable && rgb10 && bt2020 && pq && full_range;
  }
}  // namespace platf::gamescope_hdr

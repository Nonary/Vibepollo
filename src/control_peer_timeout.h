/**
 * @file src/control_peer_timeout.h
 * @brief Bounded parsing for the optional ENet control peer timeout.
 */
#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace config {
  // ENet's timestamp comparisons use a 24-hour overflow window. Keep this
  // option below that window rather than accepting the entire uint32 range.
  inline constexpr std::chrono::milliseconds max_control_peer_timeout {86'399'999};

  inline std::optional<std::chrono::milliseconds> parse_control_peer_timeout(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
      return std::nullopt;
    }

    std::uint32_t timeout_ms = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), timeout_ms);
    if (error != std::errc {} || end != value.data() + value.size() || timeout_ms > max_control_peer_timeout.count()) {
      return std::nullopt;
    }
    return std::chrono::milliseconds {timeout_ms};
  }
}  // namespace config

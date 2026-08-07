/**
 * @file src/logging_policy.h
 * @brief Deterministic policy for rendering logging records.
 */
#pragma once

#include <string>
#include <string_view>

namespace logging::policy {
  enum class level: int {
    verbose = 0,
    debug = 1,
    info = 2,
    warning = 3,
    error = 4,
    fatal = 5,
    tests = 10,
  };

  struct record_t {
    int severity = static_cast<int>(level::error);
    std::string_view message = "<missing log message>";
  };

  std::string_view severity_prefix(int severity) noexcept;
  std::string format_line(std::string_view timestamp, const record_t &record);
}  // namespace logging::policy

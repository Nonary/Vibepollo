/**
 * @file src/logging_policy.cpp
 * @brief Definitions for deterministic logging record rendering.
 */
#include "logging_policy.h"

namespace logging::policy {
  std::string_view severity_prefix(int severity) noexcept {
    switch (severity) {
      case static_cast<int>(level::verbose):
        return "Verbose: ";
      case static_cast<int>(level::debug):
        return "Debug: ";
      case static_cast<int>(level::info):
        return "Info: ";
      case static_cast<int>(level::warning):
        return "Warning: ";
      case static_cast<int>(level::error):
        return "Error: ";
      case static_cast<int>(level::fatal):
        return "Fatal: ";
#ifdef SUNSHINE_TESTS
      case static_cast<int>(level::tests):
        return "Tests: ";
#endif
      default:
        return "Log: ";
    }
  }

  std::string format_line(std::string_view timestamp, const record_t &record) {
    const auto prefix = severity_prefix(record.severity);
    std::string line;
    line.reserve(timestamp.size() + prefix.size() + record.message.size() + 5);
    line.push_back('[');
    line.append(timestamp.data(), timestamp.size());
    line.append("]: ");
    line.append(prefix.data(), prefix.size());
    line.append(record.message.data(), record.message.size());
    return line;
  }
}  // namespace logging::policy

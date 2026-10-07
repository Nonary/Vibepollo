/**
 * @file tests/unit/test_logging.cpp
 * @brief Test the production logging record rendering policy.
 */
#include "../tests_common.h"

#include <array>
#include <string>
#include <string_view>
#include <tuple>

#include <src/logging_policy.h>

namespace {
  std::array log_levels = {
    std::tuple("verbose", logging::policy::level::verbose, "Verbose: "),
    std::tuple("debug", logging::policy::level::debug, "Debug: "),
    std::tuple("info", logging::policy::level::info, "Info: "),
    std::tuple("warning", logging::policy::level::warning, "Warning: "),
    std::tuple("error", logging::policy::level::error, "Error: "),
    std::tuple("fatal", logging::policy::level::fatal, "Fatal: "),
  };

  constexpr std::string_view timestamp = "2026-08-07 00:00:00.123";
  constexpr std::string_view message = "logging policy message";
}  // namespace

struct LogLevelsTest: testing::TestWithParam<decltype(log_levels)::value_type> {};

INSTANTIATE_TEST_SUITE_P(
  Logging,
  LogLevelsTest,
  testing::ValuesIn(log_levels),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(LogLevelsTest, PutMessage) {
  const auto [label, level, prefix] = GetParam();
  const logging::policy::record_t record {static_cast<int>(level), message};

  EXPECT_EQ(
    logging::policy::format_line(timestamp, record),
    std::string("[") + std::string(timestamp) + "]: " + prefix + std::string(message)
  );
}

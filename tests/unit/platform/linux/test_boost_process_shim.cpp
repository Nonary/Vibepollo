/**
 * @file tests/unit/platform/linux/test_boost_process_shim.cpp
 * @brief Regression tests for Linux child-process environment ownership.
 */
#include "../../../tests_common.h"

#include <src/boost_process_shim.h>

#include <algorithm>
#include <string>

TEST(BoostProcessShim, ConvertedEnvironmentOwnsItsEntries) {
  auto environment = boost_process_shim::environment::current();
  environment["VIBESHINE_ENV_OWNERSHIP_TEST"] = "preserved";

  auto process_environment = environment.to_process_environment();

  const auto owned_entry = std::find_if(
    process_environment.env_buffer.cbegin(),
    process_environment.env_buffer.cend(),
    [](const auto &entry) {
      return entry.native() == "VIBESHINE_ENV_OWNERSHIP_TEST=preserved";
    }
  );

  ASSERT_NE(owned_entry, process_environment.env_buffer.cend());
}

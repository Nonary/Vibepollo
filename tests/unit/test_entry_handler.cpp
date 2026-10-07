/**
 * @file tests/unit/test_entry_handler.cpp
 * @brief Deterministic publisher metadata formatting contracts.
 */
#include "../tests_common.h"

#include <src/entry_metadata.h>

namespace {
  TEST(EntryMetadata, PublisherValuesAreFormattedForStartupLogs) {
    const auto lines = entry_metadata::publisher_log_lines({
      "Test Publisher",
      "https://example.test/publisher",
      "https://example.test/support",
    });

    EXPECT_EQ(lines[0], "Package Publisher: Test Publisher");
    EXPECT_EQ(lines[1], "Publisher Website: https://example.test/publisher");
    EXPECT_EQ(lines[2], "Get support: https://example.test/support");
  }
}  // namespace

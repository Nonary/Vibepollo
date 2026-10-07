/**
 * @file tests/unit/test_config_catalog_contract.cpp
 * @brief Portable unit contract for configure-time configuration catalogs.
 */
#include "../tests_common.h"

#include "config_catalog_contract.generated.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using option_set = std::set<std::string, std::less<>>;

  template<typename Range>
  option_set as_option_set(const Range &values) {
    option_set options;
    for (const std::string_view value : values) {
      options.emplace(value);
    }
    return options;
  }

  std::vector<std::string> missing_public_contracts(
    const option_set &config_options,
    const option_set &documented_options,
    const option_set &translated_options) {
    const option_set internal_options {
      "flags",  // Internal config flags, not user-configurable.
      "rtss_disable_vsync_ullm"  // Legacy alias for frame_limiter_disable_vsync.
    };

    std::vector<std::string> missing;
    for (const auto &option : config_options) {
      if (internal_options.contains(option)) {
        continue;
      }
      if (!documented_options.contains(option)) {
        missing.emplace_back("configuration.md missing: " + option);
      }
      if (!translated_options.contains(option)) {
        missing.emplace_back("en.json missing: " + option);
      }
    }
    return missing;
  }

}  // namespace

TEST(ConfigConsistency, PublicConfigOptionsAreDocumentedAndTranslated) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto documented_options = as_option_set(sunshine::test::config_catalog_contract::documented_options);
  const auto translated_options = as_option_set(sunshine::test::config_catalog_contract::translated_options);

  ASSERT_FALSE(config_options.empty());
  ASSERT_FALSE(documented_options.empty());
  ASSERT_FALSE(translated_options.empty());

  const auto missing = missing_public_contracts(config_options, documented_options, translated_options);
  ASSERT_TRUE(missing.empty()) << [&] {
    std::string message = "Public config options missing from retained contracts:\n";
    for (const auto &entry : missing) {
      message += "  " + entry + '\n';
    }
    return message;
  }();
}

TEST(ConfigConsistency, DummyOptionsAreAbsentFromRetainedContracts) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto documented_options = as_option_set(sunshine::test::config_catalog_contract::documented_options);
  const auto translated_options = as_option_set(sunshine::test::config_catalog_contract::translated_options);
  const std::vector<std::string_view> dummy_options {
    "dummy_config_option",
    "nonexistent_setting",
    "fake_config_parameter",
    "test_dummy_option",
    "invalid_config_key"
  };

  for (const auto option : dummy_options) {
    EXPECT_FALSE(config_options.contains(option)) << option;
    EXPECT_FALSE(documented_options.contains(option)) << option;
    EXPECT_FALSE(translated_options.contains(option)) << option;
  }
}

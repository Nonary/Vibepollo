/**
 * @file tests/integration/test_config_consistency.cpp
 * @brief Test configuration, documentation, and translation consistency.
 */
#include "../tests_common.h"

#include <format>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/file_handler.h"

namespace {

using option_set = std::set<std::string, std::less<>>;

option_set extract_config_cpp_options() {
  option_set options;
  const std::string content = file_handler::read_file("src/config.cpp");

  const std::vector patterns = {
    std::regex(R"DELIM((?:string_f|path_f|string_restricted_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM((?:int_f|int_between_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM(bool_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM((?:double_f|double_between_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM(generic_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM(list_prep_cmd_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
    std::regex(R"DELIM(map_int_int_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM")
  };

  for (const auto &pattern : patterns) {
    for (std::sregex_iterator iter(content.begin(), content.end(), pattern), end; iter != end; ++iter) {
      options.insert((*iter)[1].str());
    }
  }

  return options;
}

void trim_whitespace(std::string &value) {
  const auto last = value.find_last_not_of(" \t\r\n");
  if (last == std::string::npos) {
    value.clear();
    return;
  }
  value.erase(last + 1);
}

option_set extract_documented_options() {
  option_set options;
  const std::string content = file_handler::read_file("docs/configuration.md");
  const std::regex option_pattern(R"(^### ([^#\r\n]+))");
  std::istringstream stream(content);
  std::string line;

  while (std::getline(stream, line)) {
    std::smatch match;
    if (!std::regex_search(line, match, option_pattern)) {
      continue;
    }

    std::string option = match[1].str();
    trim_whitespace(option);
    options.insert(std::move(option));
  }

  return options;
}

option_set extract_translated_options() {
  option_set options;
  const auto locale = nlohmann::json::parse(file_handler::read_file("src_assets/common/assets/web/public/assets/locale/en.json"));

  if (!locale.contains("config") || !locale["config"].is_object()) {
    return options;
  }

  for (const auto &[key, _] : locale["config"].items()) {
    options.insert(key);
  }

  return options;
}

}  // namespace

TEST(ConfigConsistency, PublicConfigOptionsAreDocumentedAndTranslated) {
  const auto config_options = extract_config_cpp_options();
  const auto documented_options = extract_documented_options();
  const auto translated_options = extract_translated_options();

  ASSERT_FALSE(config_options.empty());
  ASSERT_FALSE(documented_options.empty());
  ASSERT_FALSE(translated_options.empty());

  const option_set internal_options = {
    "flags",  // Internal config flags, not user-configurable.
    "rtss_disable_vsync_ullm"  // Legacy alias for frame_limiter_disable_vsync.
  };

  std::vector<std::string> missing;
  for (const auto &option : config_options) {
    if (internal_options.contains(option)) {
      continue;
    }

    if (!documented_options.contains(option)) {
      missing.push_back(std::format("configuration.md missing: {}", option));
    }
    if (!translated_options.contains(option)) {
      missing.push_back(std::format("en.json missing: {}", option));
    }
  }

  if (!missing.empty()) {
    std::string message = "Public config options missing from retained contracts:\n";
    for (const auto &entry : missing) {
      message += std::format("  {}\n", entry);
    }
    FAIL() << message;
  }
}

TEST(ConfigConsistency, DummyOptionsAreAbsentFromRetainedContracts) {
  const auto config_options = extract_config_cpp_options();
  const auto documented_options = extract_documented_options();
  const auto translated_options = extract_translated_options();
  const std::vector<std::string> dummy_options = {
    "dummy_config_option",
    "nonexistent_setting",
    "fake_config_parameter",
    "test_dummy_option",
    "invalid_config_key"
  };

  for (const auto &option : dummy_options) {
    EXPECT_FALSE(config_options.contains(option)) << option;
    EXPECT_FALSE(documented_options.contains(option)) << option;
    EXPECT_FALSE(translated_options.contains(option)) << option;
  }
}

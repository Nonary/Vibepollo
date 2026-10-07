/**
 * @file tests/integration/test_locale_consistency.cpp
 * @brief Test locale JSON files against the backend locale configuration.
 */
#include "../tests_common.h"

#include <filesystem>
#include <format>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/file_handler.h"

namespace {

namespace fs = std::filesystem;
using locale_set = std::set<std::string, std::less<>>;

locale_set extract_config_cpp_locales() {
  locale_set locales;
  const std::string content = file_handler::read_file("src/config.cpp");
  const std::regex locale_section(R"(string_restricted_f\s*\(\s*vars\s*,\s*"locale"[^}]*\{([^}]*)\})");
  std::smatch match;

  if (!std::regex_search(content, match, locale_section)) {
    return locales;
  }

  const std::regex locale_pattern(R"delimiter("([^"]+)"sv)delimiter");
  for (std::sregex_iterator iter(match[1].first, match[1].second, locale_pattern), end; iter != end; ++iter) {
    locales.insert((*iter)[1].str());
  }

  return locales;
}

locale_set get_available_locale_files() {
  locale_set locales;
  const fs::path locale_dir = "src_assets/common/assets/web/public/assets/locale";

  if (!fs::exists(locale_dir)) {
    return locales;
  }

  for (const auto &entry : fs::directory_iterator(locale_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      locales.insert(entry.path().stem().string());
    }
  }

  return locales;
}

bool is_valid_locale_file(const std::string &locale) {
  const std::string path = std::format("src_assets/common/assets/web/public/assets/locale/{}.json", locale);

  try {
    const auto parsed = nlohmann::json::parse(file_handler::read_file(path.c_str()));
    return parsed.is_object() && !parsed.empty();
  } catch (const nlohmann::json::parse_error &) {
    return false;
  }
}

}  // namespace

TEST(LocaleConsistency, BackendLocalesAndLocaleFilesMatch) {
  const auto config_locales = extract_config_cpp_locales();
  const auto locale_files = get_available_locale_files();

  ASSERT_FALSE(config_locales.empty());
  ASSERT_FALSE(locale_files.empty());

  std::vector<std::string> missing;
  for (const auto &locale : config_locales) {
    if (!locale_files.contains(locale)) {
      missing.push_back(std::format("config.cpp references missing file: {}.json", locale));
    }
  }
  for (const auto &locale : locale_files) {
    if (!config_locales.contains(locale)) {
      missing.push_back(std::format("locale file has no config.cpp entry: {}.json", locale));
    }
  }

  if (!missing.empty()) {
    std::string message = "Backend locale configuration and locale JSON files differ:\n";
    for (const auto &entry : missing) {
      message += std::format("  {}\n", entry);
    }
    FAIL() << message;
  }
}

TEST(LocaleConsistency, LocaleFilesAreValidJson) {
  const auto locale_files = get_available_locale_files();
  ASSERT_FALSE(locale_files.empty());

  std::vector<std::string> invalid;
  for (const auto &locale : locale_files) {
    if (!is_valid_locale_file(locale)) {
      invalid.push_back(locale);
    }
  }

  if (!invalid.empty()) {
    std::string message = "Invalid locale JSON files:\n";
    for (const auto &locale : invalid) {
      message += std::format("  {}.json\n", locale);
    }
    FAIL() << message;
  }
}

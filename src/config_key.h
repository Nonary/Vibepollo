#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace config {
  inline std::string normalize_config_key(std::string key) {
    const auto first_ascii = std::find_if(key.begin(), key.end(), [](unsigned char ch) {
      return std::isalnum(ch) || ch == '_';
    });
    key.erase(key.begin(), first_ascii);
    return key;
  }
}  // namespace config

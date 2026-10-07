/**
 * @file src/entry_metadata.h
 * @brief Pure formatting for publisher metadata logged at startup.
 */
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace entry_metadata {
  struct publisher_data_t {
    std::string_view name;
    std::string_view website;
    std::string_view issue_url;
  };

  using publisher_log_lines_t = std::array<std::string, 3>;

  publisher_log_lines_t publisher_log_lines(publisher_data_t publisher_data);
}  // namespace entry_metadata

/**
 * @file src/entry_metadata.cpp
 * @brief Pure formatting for publisher metadata logged at startup.
 */

#include "entry_metadata.h"

namespace entry_metadata {
  publisher_log_lines_t publisher_log_lines(const publisher_data_t publisher_data) {
    return {
      "Package Publisher: " + std::string {publisher_data.name},
      "Publisher Website: " + std::string {publisher_data.website},
      "Get support: " + std::string {publisher_data.issue_url},
    };
  }
}  // namespace entry_metadata

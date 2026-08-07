/**
 * @file src/state_storage_policy.cpp
 * @brief Callback-driven JSON state-file recovery and write policy.
 */

#include "state_storage_policy.h"

#include <boost/property_tree/json_parser.hpp>

#include <sstream>
#include <stdexcept>
#include <string_view>

namespace statefile::policy {
  namespace {
    namespace pt = boost::property_tree;
    using namespace std::literals;

    bool parses_as_json(const std::string &contents) {
      try {
        pt::ptree parsed;
        std::istringstream in(contents);
        pt::read_json(in, parsed);
        return true;
      } catch (...) {
        return false;
      }
    }
  }  // namespace

  load_result_e load_json_for_update(
    const std::string &path,
    pt::ptree &tree,
    const read_file_t &read_file,
    const quarantine_file_t &quarantine_file) {
    tree = {};
    if (path.empty() || !read_file) {
      return load_result_e::failed;
    }

    auto result = read_file(path);
    if (result.status == read_status_e::missing) {
      return load_result_e::missing;
    }
    if (result.status != read_status_e::loaded) {
      return load_result_e::failed;
    }

    auto &contents = result.contents;
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
      contents.erase(0, 3);
    }
    if (contents.find_first_not_of(" \t\r\n\f\v"sv) == std::string::npos) {
      return load_result_e::missing;
    }

    try {
      std::istringstream in(contents);
      pt::read_json(in, tree);
      return load_result_e::loaded;
    } catch (...) {
      tree = {};
      if (quarantine_file) {
        quarantine_file(path);
      }
      return load_result_e::corrupt;
    }
  }

  void write_json_atomic(
    const std::string &path,
    const pt::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file) {
    if (path.empty()) {
      throw std::runtime_error("atomic JSON write path is empty");
    }
    if (!write_file || !read_file) {
      throw std::runtime_error("atomic JSON storage operations are unavailable");
    }

    std::ostringstream out;
    pt::write_json(out, tree);
    const auto contents = out.str();
    if (!parses_as_json(contents)) {
      throw std::runtime_error("refusing to write malformed JSON");
    }
    if (!write_file(path, contents)) {
      throw std::runtime_error("atomic JSON write failed");
    }

    const auto written = read_file(path);
    if (written.status != read_status_e::loaded || !parses_as_json(written.contents)) {
      throw std::runtime_error("atomic JSON write verification failed");
    }
  }
}  // namespace statefile::policy

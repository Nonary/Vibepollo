/**
 * @file src/state_storage_policy.h
 * @brief Callback-driven JSON state-file recovery and write policy.
 */
#pragma once

#include <boost/property_tree/ptree_fwd.hpp>

#include <functional>
#include <string>

namespace statefile::policy {
  enum class read_status_e {
    loaded,
    missing,
    failed,
  };

  struct read_result_t {
    read_status_e status;
    std::string contents;
  };

  enum class load_result_e {
    loaded,
    missing,
    corrupt,
    failed,
  };

  using read_file_t = std::function<read_result_t(const std::string &path)>;
  using quarantine_file_t = std::function<void(const std::string &path)>;
  using write_file_t = std::function<bool(const std::string &path, const std::string &contents)>;

  /**
   * @brief Parses recoverable state content using caller-supplied storage operations.
   *
   * Missing and blank content are safe to replace. Malformed content is delegated to
   * @p quarantine before returning an empty tree. Failed reads remain non-destructive.
   */
  load_result_e load_json_for_update(
    const std::string &path,
    boost::property_tree::ptree &tree,
    const read_file_t &read_file,
    const quarantine_file_t &quarantine_file);

  /**
   * @brief Serializes, writes, and re-parses JSON through injected storage operations.
   */
  void write_json_atomic(
    const std::string &path,
    const boost::property_tree::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file);
}  // namespace statefile::policy

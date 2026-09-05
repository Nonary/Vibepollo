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
   * @brief Reads JSON without changing the source file. An existing blank file is
   *        reported as corrupt so startup cannot mint a new identity for a
   *        truncated state file.
   *
   * This is the startup path: recovery must be selected before a damaged
   * primary is moved aside, otherwise a retry could mistake the missing
   * primary for a new installation.
   */
  load_result_e load_json_for_read(
    const std::string &path,
    boost::property_tree::ptree &tree,
    const read_file_t &read_file);

  /** Read the auxiliary state, recovering and restoring a validated .bak snapshot.
   * Both files missing means a new profile. Unrecoverable or inaccessible state
   * fails closed so a metadata update cannot erase authentication state.
   */
  load_result_e load_vibeshine_state(
    const std::string &path,
    boost::property_tree::ptree &tree,
    const read_file_t &read_file,
    const write_file_t &write_file
  );

  using validate_primary_t = std::function<bool(const boost::property_tree::ptree &, bool allow_bootstrap)>;

  /** Structural validation shared by primary reads and writes. */
  bool valid_primary_state(const boost::property_tree::ptree &tree, bool allow_bootstrap);

  /** Read/modify/write recovery must retain host identity even with shared paths. */
  load_result_e load_primary_state_for_update(
    const std::string &path,
    boost::property_tree::ptree &tree,
    const read_file_t &read_file,
    const write_file_t &write_file,
    const validate_primary_t &validate);

  /** Never let an incomplete metadata/bootstrap write replace an existing identity. */
  bool primary_write_allowed(
    const boost::property_tree::ptree &tree,
    load_result_e backup_status,
    const boost::property_tree::ptree &backup,
    const validate_primary_t &validate);

  /** Recover the configured credential file before startup inspects credentials.
   * A valid primary always wins, including an explicit empty credential state.
   * Recovery requires a complete credential snapshot and restores its exact bytes.
   */
  load_result_e recover_credentials(
    const std::string &path,
    const read_file_t &read_file,
    const write_file_t &write_file,
    bool refresh_backup = true);

  /** Validate auxiliary state before publishing it and refreshing its backup. */
  void write_vibeshine_state(
    const std::string &path,
    const boost::property_tree::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file
  );

  /**
   * @brief Serializes, writes, and re-parses JSON through injected storage operations.
   */
  void write_json_atomic(
    const std::string &path,
    const boost::property_tree::ptree &tree,
    const write_file_t &write_file,
    const read_file_t &read_file);
}  // namespace statefile::policy

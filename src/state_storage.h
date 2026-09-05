#pragma once

#include <boost/property_tree/ptree_fwd.hpp>
#include <nlohmann/json_fwd.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace statefile {

  enum class json_load_result_e {
    loaded,
    missing,
    corrupt,
    failed,
  };

  const std::string &sunshine_state_path();

  std::string sunshine_state_backup_path();

  const std::string &vibeshine_state_path();

  std::mutex &state_mutex();

  bool share_state_file();

  /**
   * @brief Write a property-tree JSON file through a temporary file and atomic replace.
   *
   * Callers that are updating shared Vibepollo state should still hold state_mutex()
   * around their read/modify/write transaction; this helper only prevents partial or
   * interleaved on-disk writes from leaving malformed JSON behind. Configured
   * primary and auxiliary state writes also refresh their .bak recovery copy.
   */
  void write_json_atomic(const std::string &path, const boost::property_tree::ptree &tree);

  /**
   * @brief Persist the primary Sunshine state and refresh its recovery copy.
   *
   * The backup is written only after the primary has been atomically replaced,
   * so it remains a last-known-good snapshot if a later primary write is
   * interrupted or the primary is found unreadable.
   */
  void write_sunshine_state_atomic(const boost::property_tree::ptree &tree);

  // Preserve Vibepollo JSON types in paired-client permissions and commands.
  void write_sunshine_state_atomic(const nlohmann::json &tree);
  void write_json_atomic(const std::string &path, const nlohmann::json &tree);

  /**
   * @brief Read a state file while preserving enough detail for recovery.
   *
   * Malformed or blank content is reported without changing the source; failed
   * inspection or reads are never treated as an empty state. The auxiliary state
   * is validated and recovered from its .bak copy before returning; callers hold
   * state_mutex() around reads as well as read/modify/write transactions.
   */
  json_load_result_e load_json(const std::string &path, boost::property_tree::ptree &tree);
  json_load_result_e load_json(const std::string &path, nlohmann::json &tree);

  /** Authoritative primary snapshot selection for startup and saves. Caller holds state_mutex(). */
  json_load_result_e load_primary_state(boost::property_tree::ptree &tree);
  json_load_result_e load_primary_state(nlohmann::json &tree);

  /**
   * @brief Load an existing JSON file before a read/modify/write update.
   *
   * Primary and auxiliary state recover from validated backups first and fail closed
   * when damaged state has no usable snapshot. The rules below apply to other files.
   * Returns true (with @p tree populated, or empty when there is nothing usable to
   * preserve) when it is safe for the caller to proceed and rewrite the file:
   *   - Missing or blank files yield an empty tree.
   *   - A readable-but-malformed file is quarantined (renamed aside) and yields an
   *     empty tree, so a single corrupt file can no longer permanently wedge a
   *     writer (the bad content was already unrecoverable).
   *
   * Returns false only when the file cannot be inspected or opened (I/O, permission,
   * or a non-regular path) so the caller does not overwrite state it could not read.
   */
  bool load_json_for_update(const std::string &path, boost::property_tree::ptree &tree);

  /**
   * @brief Best-effort repair for Windows config ACL inheritance.
   *
   * A previous session history build could protect the shared config directory
   * while tightening the history database. This restores inheritance on the
   * shared config directory and known mutable config/state files without
   * touching intentionally private subdirectories such as credentials.
   */
  void repair_config_permissions();

  /** Restore the configured credential file before HTTP initialization. Locks state_mutex(). */
  bool recover_credentials(const std::string &path);

  void migrate_recent_state_keys();

  /**
   * @brief Apply a restrictive, non-inherited ACL to a directory holding private key
   *        material (LocalSystem + Administrators full control, inheritance disabled).
   *
   * This is the runtime backstop for the credentials directory: it ensures the host
   * TLS private key is never left world-readable even if the installer's ACL-hardening
   * step is skipped or fails. Returns whether the restrictive ACL was applied and
   * verified; always succeeds without changing permissions on non-Windows.
   */
  [[nodiscard]] bool secure_private_directory(const std::string &path);

  /**
   * @brief Persist the snapshot exclusion device list to vibeshine_state.json.
   * @param devices List of device IDs to exclude from display snapshots.
   *
   * This is called when config is saved/applied so that the display helper
   * can read the exclusion list directly without depending on IPC from Sunshine.
   */
  void save_snapshot_exclude_devices(const std::vector<std::string> &devices);

  /**
   * @brief Load the snapshot exclusion device list from vibeshine_state.json.
   * @return The list of device IDs to exclude, or an empty vector if not found.
   */
  std::vector<std::string> load_snapshot_exclude_devices();

  /**
   * @brief Remember a Sunshine-managed virtual display device id in vibeshine_state.json.
   * @param device_id Device id of a virtual display created/resolved for a session.
   *
   * The display helper merges this list into its snapshot exclusions so virtual
   * displays are never captured into (or restored from) display baselines, even
   * when the virtual monitor uses a custom EDID the helper cannot classify.
   */
  void remember_virtual_display_device(const std::string &device_id);

  /**
   * @brief Load the remembered virtual display device ids from vibeshine_state.json.
   * @return The list of device IDs, or an empty vector if not found.
   */
  std::vector<std::string> load_virtual_display_devices();

  /**
   * @brief Persist the selected display helper engine ("legacy" or "v2") to
   *        vibeshine_state.json so the boot --restore scheduled task runs the
   *        same engine that armed it.
   */
  void save_display_helper_engine(const std::string &engine);

  /** Persist a compositor scale selected for a stable virtual-display owner. */
  void save_virtual_display_scale(const std::string &identity, double scale);

  /** Load the last compositor scale selected for a stable virtual-display owner. */
  std::optional<double> load_virtual_display_scale(const std::string &identity);

  /** Clear retained virtual-display scales when display state is reset. */
  void clear_virtual_display_scales();

}  // namespace statefile

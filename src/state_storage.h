#pragma once

#include <boost/property_tree/ptree_fwd.hpp>
#include <mutex>
#include <string>
#include <vector>

namespace statefile {

  const std::string &sunshine_state_path();

  const std::string &vibeshine_state_path();

  std::mutex &state_mutex();

  bool share_state_file();

  /**
   * @brief Write a property-tree JSON file through a temporary file and atomic replace.
   *
   * Callers that update shared state should still hold state_mutex() around
   * their read/modify/write transaction; this helper only prevents partial
   * on-disk writes from leaving malformed JSON behind.
   */
  void write_json_atomic(const std::string &path, const boost::property_tree::ptree &tree);

  /**
   * @brief Load an existing JSON file before a read/modify/write update.
   *
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

}  // namespace statefile

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
   * Missing files are treated as an empty tree and return true. Existing files
   * that cannot be inspected or parsed return false so callers do not overwrite
   * the last on-disk state with a partial replacement.
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

}  // namespace statefile

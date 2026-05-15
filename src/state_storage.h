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

}  // namespace statefile

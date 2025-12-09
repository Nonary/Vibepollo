#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace statefile {

  const std::string &sunshine_state_path();

  const std::string &vibeshine_state_path();

  std::mutex &state_mutex();

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

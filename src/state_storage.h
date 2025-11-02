#pragma once

#include <mutex>
#include <string>

namespace statefile {

  const std::string &sunshine_state_path();

  const std::string &vibeshine_state_path();

  std::mutex &state_mutex();

  bool share_state_file();

  void migrate_recent_state_keys();

}  // namespace statefile

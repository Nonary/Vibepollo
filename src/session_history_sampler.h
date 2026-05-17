/**
 * @file src/session_history_sampler.h
 * @brief Internal active-session tracking and periodic sampling support.
 */
#pragma once

#include "session_history.h"

#include <string>
#include <vector>

namespace session_history::sampler {

  void init();
  void shutdown();

  void register_session(const session_metadata_t &metadata);
  void unregister_session(const std::string &uuid);
  bool is_active(const std::string &uuid);

  std::vector<active_session_t> get_active_sessions();

}  // namespace session_history::sampler

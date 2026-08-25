/** @file src/steam_auto_sync.h */
#pragma once

#include "config_playnite.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace platf::steam::autosync {
  struct settings_t {
    bool enabled = true;
    bool auto_sync = true;
    bool remove_uninstalled = true;
    bool include_tools = false;
    std::vector<config::id_name_t> exclusions;
  };

  // Configuration is safe to call before start() and during a hot reload.
  // The worker copies settings under its mutex and never reads config globals
  // directly from its polling thread.
  void configure(settings_t settings);
  void start();
  void stop();
}

/** @file src/lutris_auto_sync.h */
#pragma once

#include "config_playnite.h"

#include <vector>

namespace platf::lutris::autosync {
  struct settings_t {
    bool enabled = true;
    bool auto_sync = true;
    bool remove_uninstalled = true;
    bool include_steam = false;
    std::vector<config::id_name_t> exclusions;
  };

  void configure(settings_t settings);
  void start();
  void stop();
}  // namespace platf::lutris::autosync

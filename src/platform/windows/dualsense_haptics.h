// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tools/dualsense_haptics/ipc.h"

#include <memory>
#include <string>
#include <vector>

namespace platf::dualsense_audio {
  class session {
  public:
    ~session();
    std::string mapping_name;
    HANDLE mapping {};
    dualsense_haptics::shared_state *state {};
  };

  // Shared ownership keeps a VHF poll alive until it finishes draining the mapping.
  std::shared_ptr<session> start();
  bool enabled();
  void set_slot(unsigned index, bool active);
  std::vector<dualsense_haptics::packet> drain(unsigned index);
  std::string wrap_command(const std::string &command);
}  // namespace platf::dualsense_audio

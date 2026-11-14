#pragma once

#include "display_helper_builder.h"
#include "remote_display_topology.h"

namespace display_helper_integration {
  // A detached rule can request primary without overriding the current
  // position. It must not disable an application's isolated layout.
  inline void apply_saved_stream_layout(
    DisplayApplyBuilder &builder,
    const remote_display_topology::saved_stream_layout_t &saved,
    const std::string &device_id,
    bool isolated
  ) {
    if (saved.position) {
      builder.mutable_topology().monitor_positions[device_id] = {saved.position->x, saved.position->y};
    }
    if (saved.primary_requested && (saved.position || !isolated)) {
      if (auto request = builder.build(); request.configuration) {
        request.configuration->m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary;
        builder.set_configuration(*request.configuration);
      }
    }
  }
}  // namespace display_helper_integration

/**
 * @file src/platform/linux/smooth_motion_policy.h
 * @brief Pure launch policy for NVIDIA Smooth Motion on Linux.
 */
#pragma once

#include "src/framegen_policy.h"

#include <string_view>

namespace platf::smooth_motion {

  struct launch_policy_t {
    bool enabled = false;
    bool use_graphics_queue = false;
  };

  inline launch_policy_t make_launch_policy(
    bool frame_generation_enabled,
    std::string_view frame_generation_provider,
    bool mangohud_enabled
  ) {
    const bool enabled =
      frame_generation_enabled &&
      framegen::normalize_provider(frame_generation_provider) == "nvidia-smooth-motion";
    return {
      .enabled = enabled,
      // NVIDIA's asynchronous presentation queue conflicts with MangoHUD's
      // Vulkan layer. The graphics queue is the documented compatibility path.
      .use_graphics_queue = enabled && mangohud_enabled,
    };
  }

}  // namespace platf::smooth_motion

/**
 * @file src/platform/linux/private_display.h
 * @brief Linux private-streaming-display lifecycle managed through KScreen.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <display_device/types.h>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace platf::linux_private_display {
  struct prepare_result_t {
    bool requested {false};
    bool active {false};
    std::string output_name;
    std::string error;
  };

  /** Disable unowned private outputs left enabled after a compositor/service crash. */
  bool initialize();

  /** Resolve virtual-display policy and reserve an exact private output for the session. */
  prepare_result_t prepare_session(
    rtsp_stream::launch_session_t &session,
    bool no_active_sessions,
    bool allow_display_changes
  );

  /** Apply resolution, refresh, HDR, scale, primary-output, and layout policy. */
  bool apply_session(const rtsp_stream::launch_session_t &session);

  /** Restore the pre-stream topology and release all private-output reservations. */
  bool revert();

  /** Forget cached topology/reservations after restoring the current session. */
  bool reset_persistence();

  /** Generation-fenced delayed restore used while an application remains paused. */
  void schedule_revert(std::chrono::milliseconds delay);
  void cancel_scheduled_revert();

  /** Runtime capability/readiness and Web UI display enumeration. */
  bool capable();
  bool ready();
  bool kernel_pool_available();
  std::vector<std::string> private_output_names();
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  );
  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail);
}  // namespace platf::linux_private_display

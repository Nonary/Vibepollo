/**
 * @file src/platform/linux/private_display.h
 * @brief Linux private-streaming-display lifecycle managed through KScreen.
 */
#pragma once

#include "src/remote_display_topology.h"

#include <chrono>
#include <display_device/types.h>
#include <optional>
#include <string>
#include <vector>

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

  /**
   * Fence process shutdown from compositor and connector mutation. The
   * request operation is a single lock-free atomic store and is safe from the
   * synchronous Linux signal-wait path before teardown begins.
   */
  void request_process_shutdown_preserve() noexcept;
  bool process_shutdown_preserve_requested() noexcept;

  /** Disable unowned private outputs left enabled after a compositor/service crash. */
  bool initialize();

  /** Resolve virtual-display policy and reserve an exact private output for the session. */
  prepare_result_t prepare_session(
    rtsp_stream::launch_session_t &session,
    bool no_active_sessions,
    bool allow_display_changes
  );

  /** Apply resolution, refresh, HDR, scale, primary-output, and layout policy. */
  bool apply_session(rtsp_stream::launch_session_t &session);
  /** Publish verified current HDR/readiness state after a composed apply. */
  bool publish_current_session_state(rtsp_stream::launch_session_t &session);

  /** Reserve or reclaim the stable private connector owned by a paired client. */
  bool remote_create_or_reclaim(
    const std::string &client_uuid,
    const remote_display_topology::mode_t &mode
  );

  /** Downgrade requested mode features not supported by the reserved output. */
  void remote_resolve_mode(
    const std::string &client_uuid,
    remote_display_topology::mode_t &mode
  );

  /** Atomically apply the coordinator's complete physical/client desktop graph. */
  bool remote_apply_composed_topology(
    const std::vector<remote_display_topology::node_t> &nodes
  );

  /** Verify that a client's exact connector has the requested mode and is capture-enumerated. */
  std::optional<std::string> remote_exact_capture_output(
    const std::string &client_uuid,
    const remote_display_topology::mode_t &mode
  );

  /** Release only this client's connector reservation; topology apply performs the disable. */
  bool remote_remove_owned_display(const std::string &client_uuid);

  /** Inspect stable ownership while constructing the Remote Monitor baseline. */
  bool is_private_output(const std::string &output_name);
  std::optional<std::string> output_for_client(const std::string &client_uuid);

  /** Restore the pre-stream topology and release all private-output reservations. */
  bool revert();

  /** Forget cached topology/reservations after restoring the current session. */
  bool reset_persistence();

  /** Generation-fenced delayed restore with a lifecycle-specific diagnostic reason. */
  void schedule_revert(std::chrono::milliseconds delay, std::string reason);
  void cancel_scheduled_revert();

  /** Runtime capability/readiness and Web UI display enumeration. */
  bool capable();
  bool ready();
  bool hdr_capable();
  /** Detect an HDR-capable private kernel connector without requiring KScreen state. */
  bool kernel_hdr_pool_available();
  bool kernel_pool_available();
  std::vector<std::string> private_output_names();
  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    display_device::DeviceEnumerationDetail detail
  );
  std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail);
}  // namespace platf::linux_private_display
